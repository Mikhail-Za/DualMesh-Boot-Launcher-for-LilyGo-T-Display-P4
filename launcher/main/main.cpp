/*
 * DualMesh launcher v1 — touch boot-selector + flash-from-SD for T-Display P4.
 *
 * Lives in the factory partition; the bootloader lands here whenever otadata is
 * erased/invalid. Shows what's installed in the two OTA slots, boots either on
 * tap, and installs app-only .bin images from /sdcard/firmware into a slot.
 * Serial commands (115200): list, boot0, boot1, erase-otadata, help.
 *
 * Display/touch are runtime-detected (HI8561 TFT or RM69A10 AMOLED) via the
 * LilyGo device driver, so one launcher binary serves both SKUs.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lilygo_device_driver_library.h"
#include "lvgl.h"

namespace {

constexpr const char *kTag = "launcher";
constexpr const char *kFirmwareDir = "/sdcard/firmware";
constexpr int kLvglTickMs = 2;

_lock_t g_lvgl_lock;
bool g_sd_ok = false;

lv_obj_t *g_slot_status[2];
lv_obj_t *g_slot_boot_btn[2];
lv_obj_t *g_file_list;
lv_obj_t *g_selected_label;
lv_obj_t *g_install_btn[2];
lv_obj_t *g_progress_bar;
lv_obj_t *g_progress_label;
std::string g_selected_file;

/* ---------- slot inspection / boot ---------- */

const esp_partition_t *SlotPartition(int slot) {
  return esp_partition_find_first(
      ESP_PARTITION_TYPE_APP,
      slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1,
      NULL);
}

std::string DescribeSlot(int slot) {
  const esp_partition_t *part = SlotPartition(slot);
  if (part == NULL) return "partition missing";

  uint8_t magic = 0;
  esp_partition_read(part, 0, &magic, 1);
  if (magic != ESP_IMAGE_HEADER_MAGIC) return "(empty)";

  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
    return std::string(desc.project_name) + "  " + desc.version;
  }
  return "(image present, no descriptor)";
}

bool SlotBootable(int slot) {
  const esp_partition_t *part = SlotPartition(slot);
  if (part == NULL) return false;
  uint8_t magic = 0;
  esp_partition_read(part, 0, &magic, 1);
  return magic == ESP_IMAGE_HEADER_MAGIC;
}

void BootSlot(int slot) {
  const esp_partition_t *part = SlotPartition(slot);
  if (part == NULL) return;
  printf("[launcher] verifying + switching to %s...\n", part->label);
  esp_err_t err = esp_ota_set_boot_partition(part);  // verifies image first
  if (err != ESP_OK) {
    printf("[launcher] set_boot_partition failed: %s\n", esp_err_to_name(err));
    return;
  }
  printf("[launcher] rebooting into %s\n", part->label);
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}

/* ---------- install-from-SD worker ---------- */

struct InstallJob {
  std::string path;
  int slot;
};

void SetProgress(const char *text, int pct) {
  _lock_acquire(&g_lvgl_lock);
  lv_label_set_text(g_progress_label, text);
  if (pct >= 0) {
    lv_bar_set_value(g_progress_bar, pct, LV_ANIM_OFF);
    lv_obj_clear_flag(g_progress_bar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_progress_bar, LV_OBJ_FLAG_HIDDEN);
  }
  _lock_release(&g_lvgl_lock);
}

void RefreshSlotsUi();

void InstallTask(void *arg) {
  InstallJob *job = static_cast<InstallJob *>(arg);
  const esp_partition_t *part = SlotPartition(job->slot);

  FILE *f = fopen(job->path.c_str(), "rb");
  do {
    if (f == NULL || part == NULL) {
      SetProgress("ERROR: cannot open file/slot", -1);
      break;
    }
    struct stat st;
    stat(job->path.c_str(), &st);
    size_t fsize = st.st_size;
    if (fsize == 0 || fsize > part->size) {
      SetProgress("ERROR: file empty or larger than slot", -1);
      break;
    }
    uint8_t first = 0;
    fread(&first, 1, 1, f);
    fseek(f, 0, SEEK_SET);
    if (first != ESP_IMAGE_HEADER_MAGIC) {
      SetProgress("ERROR: not an app image (no 0xE9 magic)", -1);
      break;
    }

    size_t erase_len = (fsize + 0xFFF) & ~static_cast<size_t>(0xFFF);
    SetProgress("Erasing slot...", 0);
    if (esp_partition_erase_range(part, 0, erase_len) != ESP_OK) {
      SetProgress("ERROR: erase failed", -1);
      break;
    }

    constexpr size_t kChunk = 64 * 1024;
    std::vector<uint8_t> buf(kChunk);
    size_t written = 0;
    bool fail = false;
    while (written < fsize) {
      size_t n = fread(buf.data(), 1, kChunk, f);
      if (n == 0) break;
      if (esp_partition_write(part, written, buf.data(), n) != ESP_OK) {
        fail = true;
        break;
      }
      written += n;
      char msg[64];
      snprintf(msg, sizeof(msg), "Writing %u / %u KB", (unsigned)(written / 1024),
               (unsigned)(fsize / 1024));
      SetProgress(msg, (int)(written * 100 / fsize));
    }

    if (fail || written != fsize) {
      SetProgress("ERROR: write failed", -1);
      break;
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
      char msg[96];
      snprintf(msg, sizeof(msg), "Installed: %s %s", desc.project_name, desc.version);
      SetProgress(msg, 100);
    } else {
      SetProgress("Installed (no app descriptor?)", 100);
    }
  } while (false);

  if (f) fclose(f);
  delete job;

  _lock_acquire(&g_lvgl_lock);
  RefreshSlotsUi();
  for (int i = 0; i < 2; i++) lv_obj_clear_state(g_install_btn[i], LV_STATE_DISABLED);
  _lock_release(&g_lvgl_lock);
  vTaskDelete(NULL);
}

/* ---------- UI ---------- */

void RefreshSlotsUi() {
  const char *names[2] = {"Slot 0 - MeshCore bay", "Slot 1 - Meshtastic bay"};
  for (int i = 0; i < 2; i++) {
    std::string d = DescribeSlot(i);
    lv_label_set_text_fmt(g_slot_status[i], "%s\n%s", names[i], d.c_str());
    if (SlotBootable(i)) {
      lv_obj_clear_state(g_slot_boot_btn[i], LV_STATE_DISABLED);
    } else {
      lv_obj_add_state(g_slot_boot_btn[i], LV_STATE_DISABLED);
    }
  }
}

void RefreshFileList() {
  lv_obj_clean(g_file_list);
  if (!g_sd_ok) {
    lv_list_add_text(g_file_list, "SD card not mounted");
    return;
  }
  DIR *dir = opendir(kFirmwareDir);
  if (dir == NULL) {
    lv_list_add_text(g_file_list, "/sdcard/firmware not found");
    return;
  }
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    const char *dot = strrchr(ent->d_name, '.');
    if (dot == NULL || strcasecmp(dot, ".bin") != 0) continue;
    lv_obj_t *btn = lv_list_add_button(g_file_list, LV_SYMBOL_FILE, ent->d_name);
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t *e) {
          lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
          g_selected_file = std::string(kFirmwareDir) + "/" +
                            lv_list_get_button_text(g_file_list, btn);
          lv_label_set_text_fmt(g_selected_label, "Selected: %s",
                                lv_list_get_button_text(g_file_list, btn));
        },
        LV_EVENT_CLICKED, NULL);
    count++;
  }
  closedir(dir);
  if (count == 0) lv_list_add_text(g_file_list, "no .bin files in /sdcard/firmware");
}

void StartInstall(int slot) {
  if (g_selected_file.empty()) {
    lv_label_set_text(g_progress_label, "Select a .bin file first");
    return;
  }
  for (int i = 0; i < 2; i++) lv_obj_add_state(g_install_btn[i], LV_STATE_DISABLED);
  InstallJob *job = new InstallJob{g_selected_file, slot};
  xTaskCreate(InstallTask, "install", 8192, job, 4, NULL);
}

void BuildUi(lv_display_t *display) {
  lv_obj_t *scr = lv_display_get_screen_active(display);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(scr, 16, 0);
  lv_obj_set_style_pad_row(scr, 14, 0);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "DualMesh Launcher");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8EDF2), 0);

  for (int i = 0; i < 2; i++) {
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C242C), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    g_slot_status[i] = lv_label_create(card);
    lv_obj_set_style_text_color(g_slot_status[i], lv_color_hex(0xC4CDD6), 0);
    lv_obj_set_flex_grow(g_slot_status[i], 1);

    g_slot_boot_btn[i] = lv_button_create(card);
    lv_obj_set_size(g_slot_boot_btn[i], 130, 64);
    lv_obj_t *bl = lv_label_create(g_slot_boot_btn[i]);
    lv_label_set_text(bl, "BOOT");
    lv_obj_center(bl);
    static int slot_ids[2] = {0, 1};
    lv_obj_add_event_cb(
        g_slot_boot_btn[i],
        [](lv_event_t *e) { BootSlot(*(int *)lv_event_get_user_data(e)); },
        LV_EVENT_CLICKED, &slot_ids[i]);
  }

  lv_obj_t *sd_title = lv_label_create(scr);
  lv_label_set_text(sd_title, "Install from SD  (/firmware)");
  lv_obj_set_style_text_color(sd_title, lv_color_hex(0xE8EDF2), 0);
  lv_obj_set_style_text_font(sd_title, &lv_font_montserrat_24, 0);

  g_file_list = lv_list_create(scr);
  lv_obj_set_size(g_file_list, lv_pct(100), 380);
  lv_obj_set_style_bg_color(g_file_list, lv_color_hex(0x1C242C), 0);

  g_selected_label = lv_label_create(scr);
  lv_label_set_text(g_selected_label, "Selected: (none)");
  lv_obj_set_style_text_color(g_selected_label, lv_color_hex(0xC4CDD6), 0);

  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  const char *install_names[2] = {"Install > Slot 0", "Install > Slot 1"};
  for (int i = 0; i < 2; i++) {
    g_install_btn[i] = lv_button_create(row);
    lv_obj_set_size(g_install_btn[i], 220, 60);
    lv_obj_t *il = lv_label_create(g_install_btn[i]);
    lv_label_set_text(il, install_names[i]);
    lv_obj_center(il);
    static int slot_ids2[2] = {0, 1};
    lv_obj_add_event_cb(
        g_install_btn[i],
        [](lv_event_t *e) { StartInstall(*(int *)lv_event_get_user_data(e)); },
        LV_EVENT_CLICKED, &slot_ids2[i]);
  }

  g_progress_bar = lv_bar_create(scr);
  lv_obj_set_size(g_progress_bar, lv_pct(100), 18);
  lv_bar_set_range(g_progress_bar, 0, 100);
  lv_obj_add_flag(g_progress_bar, LV_OBJ_FLAG_HIDDEN);

  g_progress_label = lv_label_create(scr);
  lv_label_set_text(g_progress_label, "");
  lv_obj_set_style_text_color(g_progress_label, lv_color_hex(0x8FD18C), 0);

  RefreshSlotsUi();
  RefreshFileList();
}

/* ---------- display plumbing ---------- */

void LvglTask(void *arg) {
  for (;;) {
    _lock_acquire(&g_lvgl_lock);
    uint32_t next = lv_timer_handler();
    _lock_release(&g_lvgl_lock);
    if (next < 10) next = 10;
    vTaskDelay(pdMS_TO_TICKS(next));
  }
}

void SerialTask(void *arg) {
  char line[64];
  size_t pos = 0;
  for (;;) {
    int c = getchar();
    if (c == EOF) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (pos == 0) continue;
      line[pos] = '\0';
      pos = 0;
      if (strcmp(line, "list") == 0 || strcmp(line, "help") == 0) {
        printf("slot0: %s\nslot1: %s\n", DescribeSlot(0).c_str(),
               DescribeSlot(1).c_str());
      } else if (strcmp(line, "boot0") == 0) {
        BootSlot(0);
      } else if (strcmp(line, "boot1") == 0) {
        BootSlot(1);
      } else if (strcmp(line, "erase-otadata") == 0) {
        const esp_partition_t *otadata = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
        if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);
        printf("otadata erased\n");
      } else {
        printf("unknown command: '%s'\n", line);
      }
    } else if (pos < sizeof(line) - 1) {
      line[pos++] = (char)c;
    }
  }
}

}  // namespace

extern "C" void app_main(void) {
  auto &driver = lilygo_device_driver::TDisplayP4Driver::GetInstance();

  driver.CreateDrivers();
  driver.InitPower();
  driver.InitXl9535();
  driver.ConfigXl9535();
  driver.InitSgm38121();
  bool screen_ok = driver.InitScreen();
  bool touch_ok = driver.InitTouch();
  driver.InitScreenBacklight();
  g_sd_ok = driver.InitSdmmc("/sdcard");
  printf("[launcher] screen=%d touch=%d sd=%d type=%d\n", screen_ok, touch_ok, g_sd_ok,
         (int)driver.screen_type());

  xTaskCreate(SerialTask, "serial", 4096, NULL, 3, NULL);

  if (!screen_ok) {
    printf("[launcher] no display — headless mode, serial commands active\n");
    return;
  }

  const auto &info = driver.screen_info();
  const bool is_tft = driver.screen_type() ==
                      lilygo_device_driver::t_display_p4::device::ScreenType::kHi8561;

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000); });

  lv_display_t *display = lv_display_create(info.width, info.height);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);

  size_t buf_size = (size_t)info.width * info.height * sizeof(lv_color_t);
  void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
  assert(buf);
  lv_display_set_buffers(display, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

  if (is_tft) {
    lv_display_set_user_data(display, driver.chip().hi8561.get());
    lv_display_set_flush_cb(display, [](lv_display_t *disp, const lv_area_t *area,
                                        uint8_t *px_map) {
      auto *screen = (cpp_bus_driver::Hi8561 *)lv_display_get_user_data(disp);
      screen->SendColorStreamCoordinate(area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                                        px_map);
    });
  } else {
    lv_display_set_user_data(display, driver.chip().rm69a10.get());
    lv_display_set_flush_cb(display, [](lv_display_t *disp, const lv_area_t *area,
                                        uint8_t *px_map) {
      auto *screen = (cpp_bus_driver::Rm69a10 *)lv_display_get_user_data(disp);
      screen->SendColorStreamCoordinate(area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                                        px_map);
    });
  }

  if (touch_ok) {
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, (void *)(intptr_t)is_tft);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data) {
      auto &drv = lilygo_device_driver::TDisplayP4Driver::GetInstance();
      bool tft = (bool)(intptr_t)lv_indev_get_user_data(indev);
      data->state = LV_INDEV_STATE_REL;
      if (tft) {
        cpp_bus_driver::Hi8561Touch::TouchPoint tp;
        if (drv.chip().hi8561_touch->GetSingleTouchPoint(tp)) {
          data->state = LV_INDEV_STATE_PR;
          data->point.x = tp.info[0].x;
          data->point.y = tp.info[0].y;
        }
      } else {
        cpp_bus_driver::Gt9895::TouchPoint tp;
        if (drv.chip().gt9895->GetSingleTouchPoint(tp)) {
          data->state = LV_INDEV_STATE_PR;
          data->point.x = tp.info[0].x;
          data->point.y = tp.info[0].y;
        }
      }
    });
  }

  esp_lcd_dpi_panel_event_callbacks_t cbs = {
      .on_color_trans_done =
          [](esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *,
             void *user_ctx) -> bool {
        lv_display_flush_ready((lv_display_t *)user_ctx);
        return false;
      },
      .on_refresh_done = NULL,
  };
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(
      driver.bus().screen_mipi_bus->device_handle(), &cbs, display));

  _lock_acquire(&g_lvgl_lock);
  BuildUi(display);
  _lock_release(&g_lvgl_lock);

  if (is_tft) {
    driver.chip().hi8561_backlight->StartGradientTime(100, 500);
  } else {
    for (uint8_t b = 0; b < 255; b += 5) {
      driver.chip().rm69a10->SetBrightness(b);
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  xTaskCreate(LvglTask, "lvgl", 8192, NULL, 5, NULL);
}
