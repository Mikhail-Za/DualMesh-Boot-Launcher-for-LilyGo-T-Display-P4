/* DualMesh C6 updater — runs ONE-SHOT from the flex bay (ota_1) under the
 * DualMesh launcher. Connects to the ESP32-C6 over SDIO (esp-hosted), reports
 * the slave's current firmware version, and on serial command "flash" OTAs the
 * embedded network_adapter image, then verifies the new version.
 *
 * Safety posture:
 *  - Does NOTHING destructive without the explicit "flash" command at 115200.
 *  - Logs the pre-flash slave version loudly: that is the rollback target.
 *  - Never cancels OTA rollback, so the next reset returns to the launcher.
 *
 * The C6 enable line sits on the XL9535 expander (bit 12) where the hosted
 * driver's gpio reset cannot reach; we power-cycle the C6 ourselves before
 * hosted init (read-modify-write — other expander bits carry power rails and
 * the SX1262 reset, which must not be disturbed).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_hosted.h"

static const char *TAG = "c6-updater";

extern const uint8_t slave_fw_start[] asm("_binary_slave_fw_bin_start");
extern const uint8_t slave_fw_end[] asm("_binary_slave_fw_bin_end");

/* XL9535 @0x20, I2C0 SDA=7 SCL=8. Registers: OUT0=0x02 OUT1=0x03 CFG0=0x06
 * CFG1=0x07 (config: 1=input, 0=output). Expander bit 12 = port1 bit 4 = C6 EN. */
static i2c_master_dev_handle_t xl9535;

static esp_err_t xl_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(xl9535, &reg, 1, val, 1, 100);
}

static esp_err_t xl_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(xl9535, buf, 2, 100);
}

static void xl_update(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t v = 0;
    if (xl_read(reg, &v) != ESP_OK) {
        ESP_LOGE(TAG, "XL9535 read reg 0x%02x failed", reg);
        return;
    }
    v = (v & ~clear_mask) | set_mask;
    if (xl_write(reg, v) != ESP_OK) {
        ESP_LOGE(TAG, "XL9535 write reg 0x%02x failed", reg);
    }
}

static void board_power_and_c6_cycle(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = 7,
        .scl_io_num = 8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &xl9535));

    /* Port0: bit0 = 3V3 EN (low=on), bit6 = 5V EN (high=on) -> outputs */
    xl_update(0x06, (1 << 0) | (1 << 6), 0);          /* cfg: outputs */
    xl_update(0x02, (1 << 0), (1 << 6));              /* 3V3 on, 5V on */

    /* Port1: bit0 = VCCA EN (low=on), bit4 = C6 EN -> outputs */
    xl_update(0x07, (1 << 0) | (1 << 4), 0);          /* cfg: outputs */
    xl_update(0x03, (1 << 0), 0);                     /* VCCA on */

    ESP_LOGI(TAG, "power rails set; power-cycling C6 (expander bit 12)...");
    xl_update(0x03, (1 << 4), 0);                     /* C6 EN low */
    vTaskDelay(pdMS_TO_TICKS(200));
    xl_update(0x03, 0, (1 << 4));                     /* C6 EN high */
    vTaskDelay(pdMS_TO_TICKS(2500));                  /* let C6 boot */
}

static int read_line(char *buf, size_t len)
{
    size_t n = 0;
    while (n < len - 1) {
        int c = getchar();
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n == 0) continue;
            break;
        }
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n;
}

static bool query_version(esp_hosted_coprocessor_fwver_t *ver)
{
    memset(ver, 0, sizeof(*ver));
    int err = esp_hosted_get_coprocessor_fwversion(ver);
    if (err) {
        ESP_LOGE(TAG, "get_coprocessor_fwversion failed: %d", err);
        return false;
    }
    return true;
}

void app_main(void)
{
    const uint8_t *img = slave_fw_start;
    size_t img_len = slave_fw_end - slave_fw_start;
    /* Trim the 0xFF padding the partition-extraction left at the tail */
    while (img_len > 0 && img[img_len - 1] == 0xFF)
        img_len--;

    printf("\n=== DualMesh C6 updater ===\n");
    printf("embedded slave image: %u bytes (trimmed from %u)\n",
           (unsigned)img_len, (unsigned)(slave_fw_end - slave_fw_start));

    board_power_and_c6_cycle();

    ESP_LOGI(TAG, "esp_hosted_init...");
    int err = esp_hosted_init();
    if (err) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %d — NOT flashing. Reset to return to launcher.", err);
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    ESP_LOGI(TAG, "connecting to slave...");
    err = esp_hosted_connect_to_slave();
    if (err) {
        ESP_LOGE(TAG, "connect_to_slave failed: %d — NOT flashing. Reset to return to launcher.", err);
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    esp_hosted_coprocessor_fwver_t ver;
    if (query_version(&ver)) {
        printf("\n*** CURRENT C6 FIRMWARE: %lu.%lu.%lu  <-- ROLLBACK TARGET, write this down ***\n\n",
               (unsigned long)ver.major1, (unsigned long)ver.minor1, (unsigned long)ver.patch1);
    } else {
        printf("\n*** CURRENT C6 FIRMWARE: UNKNOWN (version RPC failed; link may be degraded) ***\n\n");
    }

    printf("commands: 'version' = re-query | 'flash' = OTA embedded network_adapter v2.12.7 | 'reboot'\n");

    char line[32];
    while (1) {
        printf("> ");
        fflush(stdout);
        read_line(line, sizeof(line));
        if (strcmp(line, "version") == 0) {
            if (query_version(&ver))
                printf("C6 firmware: %lu.%lu.%lu\n",
                       (unsigned long)ver.major1, (unsigned long)ver.minor1, (unsigned long)ver.patch1);
        } else if (strcmp(line, "reboot") == 0) {
            esp_restart();
        } else if (strcmp(line, "flash") == 0) {
            printf("starting slave OTA (%u bytes)...\n", (unsigned)img_len);
            err = esp_hosted_slave_ota_begin();
            if (err) {
                ESP_LOGE(TAG, "ota_begin failed: %d", err);
                continue;
            }
            const size_t CHUNK = 4096;
            size_t sent = 0, next_pct = 5;
            bool failed = false;
            while (sent < img_len) {
                size_t n = (img_len - sent) < CHUNK ? (img_len - sent) : CHUNK;
                err = esp_hosted_slave_ota_write((uint8_t *)(img + sent), n);
                if (err) {
                    ESP_LOGE(TAG, "ota_write failed at %u: %d", (unsigned)sent, err);
                    failed = true;
                    break;
                }
                sent += n;
                if (sent * 100 / img_len >= next_pct) {
                    printf("  %u%% (%u/%u)\n", (unsigned)(sent * 100 / img_len),
                           (unsigned)sent, (unsigned)img_len);
                    next_pct += 5;
                }
            }
            if (failed) continue;
            err = esp_hosted_slave_ota_end();
            if (err) {
                ESP_LOGE(TAG, "ota_end failed: %d", err);
                continue;
            }
            printf("activating new slave image (C6 will reboot)...\n");
            err = esp_hosted_slave_ota_activate();
            if (err)
                ESP_LOGE(TAG, "ota_activate failed: %d", err);
            vTaskDelay(pdMS_TO_TICKS(4000));
            if (query_version(&ver))
                printf("\n*** NEW C6 FIRMWARE: %lu.%lu.%lu ***\n",
                       (unsigned long)ver.major1, (unsigned long)ver.minor1, (unsigned long)ver.patch1);
            else
                printf("version re-query failed — reset the board and re-run 'version'\n");
        } else if (line[0]) {
            printf("unknown command: %s\n", line);
        }
    }
}
