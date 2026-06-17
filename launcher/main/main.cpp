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
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr const char *kTag = "launcher";
constexpr const char *kFirmwareDir = "/sdcard/firmware";
constexpr int kLvglTickMs = 2;
constexpr int kSplashSeconds = 3;

_lock_t g_lvgl_lock;
bool g_sd_ok = false;
bool g_nvs_ok = false;

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

/* Last-booted slot lives in the mesh_nvs partition (never in "nvs" — that one
 * belongs to MeshOS and we treat it as read-only). */
int LoadLastSlot(void) {
  if (!g_nvs_ok) return -1;
  nvs_handle_t h;
  if (nvs_open_from_partition("mesh_nvs", "launcher", NVS_READONLY, &h) != ESP_OK)
    return -1;
  int32_t v = -1;
  nvs_get_i32(h, "last_slot", &v);
  nvs_close(h);
  return v;
}

void SaveLastSlot(int slot) {
  if (!g_nvs_ok) return;
  nvs_handle_t h;
  if (nvs_open_from_partition("mesh_nvs", "launcher", NVS_READWRITE, &h) != ESP_OK)
    return;
  nvs_set_i32(h, "last_slot", slot);
  nvs_commit(h);
  nvs_close(h);
}

void BootSlot(int slot) {
  const esp_partition_t *part = SlotPartition(slot);
  if (part == NULL) return;
  printf("[launcher] verifying + switching to %s...\n", part->label);
  /* With BOOTLOADER_APP_ROLLBACK_ENABLE this is a ONE-SHOT boot: the image is
   * marked NEW, the bootloader flips it to PENDING_VERIFY, and since mesh
   * firmwares never confirm, the next reset falls back to this launcher. */
  esp_err_t err = esp_ota_set_boot_partition(part);  // verifies image first
  if (err != ESP_OK) {
    printf("[launcher] set_boot_partition failed: %s\n", esp_err_to_name(err));
    return;
  }
  SaveLastSlot(slot);
  printf("[launcher] rebooting into %s\n", part->label);
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}

/* Clear boot-selection history so the rollback fallback always lands on the
 * factory launcher (never on a stale "previous" OTA entry). */
void EraseOtadata(void) {
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);
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

void DoStartInstall(int slot) {
  for (int i = 0; i < 2; i++) lv_obj_add_state(g_install_btn[i], LV_STATE_DISABLED);
  InstallJob *job = new InstallJob{g_selected_file, slot};
  xTaskCreate(InstallTask, "install", 8192, job, 4, NULL);
}

/* Slot 0 holds licensed MeshOS, which cannot be re-downloaded — overwriting it
 * requires explicit confirmation. */
void ShowOverwriteConfirm(int slot) {
  lv_obj_t *overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x0B0E11), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
  lv_obj_set_style_border_width(overlay, 0, 0);
  lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(overlay, 24, 0);

  lv_obj_t *warn = lv_label_create(overlay);
  lv_label_set_text(warn,
                    "Slot 0 holds MeshCore (licensed).\n"
                    "It cannot be re-downloaded.\n\nOverwrite it?");
  lv_obj_set_style_text_color(warn, lv_color_hex(0xF2C14E), 0);
  lv_obj_set_style_text_font(warn, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *cancel = lv_button_create(overlay);
  lv_obj_set_size(cancel, 260, 70);
  lv_obj_t *cl = lv_label_create(cancel);
  lv_label_set_text(cl, "Cancel");
  lv_obj_center(cl);
  lv_obj_add_event_cb(
      cancel, [](lv_event_t *e) { lv_obj_delete((lv_obj_t *)lv_event_get_user_data(e)); },
      LV_EVENT_CLICKED, overlay);

  lv_obj_t *confirm = lv_button_create(overlay);
  lv_obj_set_size(confirm, 260, 70);
  lv_obj_set_style_bg_color(confirm, lv_color_hex(0xB3392E), 0);
  lv_obj_t *fl = lv_label_create(confirm);
  lv_label_set_text(fl, "Overwrite Slot 0");
  lv_obj_center(fl);
  static int confirm_slot;
  confirm_slot = slot;
  lv_obj_add_event_cb(
      confirm,
      [](lv_event_t *e) {
        lv_obj_delete((lv_obj_t *)lv_event_get_user_data(e));
        DoStartInstall(confirm_slot);
      },
      LV_EVENT_CLICKED, overlay);
}

void StartInstall(int slot) {
  if (slot == 0) {
    // ota_0 holds the licensed MeshOS. A short TAP never flashes it, so the
    // license can't be lost to a stray tap. It's locked by default; a deliberate
    // LONG-PRESS on the slot-0 button (see BuildUi) opens the overwrite confirm,
    // so someone who doesn't have/want MeshOS can use ota_0 for their own firmware.
    lv_label_set_text(g_progress_label, "Slot 0 is MeshOS - long-press the button to unlock");
    return;
  }
  if (g_selected_file.empty()) {
    lv_label_set_text(g_progress_label, "Select a .bin file first");
    return;
  }
  DoStartInstall(slot);
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
  const char *install_names[2] = {"Slot 0: MeshOS (hold to unlock)", "Install > Slot 1"};
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
  // ota_0 = MeshOS, locked by default. A short tap is refused (see StartInstall);
  // a deliberate LONG-PRESS opens the overwrite confirm, so someone who doesn't
  // want MeshOS can flash their own firmware into the slot.
  lv_obj_add_event_cb(
      g_install_btn[0], [](lv_event_t *) { ShowOverwriteConfirm(0); },
      LV_EVENT_LONG_PRESSED, NULL);

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

/* GRUB-style auto-boot splash: full-screen overlay, counts down, tap anywhere
 * to stay in the launcher. Runs only when the last-booted slot is bootable. */
struct SplashState {
  lv_obj_t *overlay;
  lv_obj_t *count_label;
  lv_timer_t *timer;
  int slot;
  int seconds_left;
};

void ShowAutoBootSplash(int slot) {
  auto *st = new SplashState{};
  st->slot = slot;
  st->seconds_left = kSplashSeconds;

  st->overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(st->overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(st->overlay, lv_color_hex(0x0B0E11), 0);
  lv_obj_set_style_bg_opa(st->overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(st->overlay, 0, 0);
  lv_obj_set_flex_flow(st->overlay, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(st->overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(st->overlay, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *title = lv_label_create(st->overlay);
  lv_label_set_text_fmt(title, "Booting %s", slot == 0 ? "MeshCore" : "Slot 1");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8EDF2), 0);

  st->count_label = lv_label_create(st->overlay);
  lv_label_set_text_fmt(st->count_label, "%d", st->seconds_left);
  lv_obj_set_style_text_font(st->count_label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(st->count_label, lv_color_hex(0x8FD18C), 0);

  lv_obj_t *hint = lv_label_create(st->overlay);
  lv_label_set_text(hint, "tap anywhere for menu");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x7A8590), 0);

  lv_obj_add_event_cb(
      st->overlay,
      [](lv_event_t *e) {
        auto *st = (SplashState *)lv_event_get_user_data(e);
        lv_timer_delete(st->timer);
        lv_obj_delete(st->overlay);
        delete st;
        printf("[launcher] auto-boot cancelled by touch\n");
      },
      LV_EVENT_CLICKED, st);

  st->timer = lv_timer_create(
      [](lv_timer_t *t) {
        auto *st = (SplashState *)lv_timer_get_user_data(t);
        st->seconds_left--;
        if (st->seconds_left <= 0) {
          lv_label_set_text(st->count_label, "boot");
          BootSlot(st->slot);  // does not return on success
          lv_timer_delete(st->timer);
          lv_obj_delete(st->overlay);
          delete st;
        } else {
          lv_label_set_text_fmt(st->count_label, "%d", st->seconds_left);
        }
      },
      1000, st);
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
  /* Always reach the launcher with a clean slate: any stale boot selection is
   * erased so the rollback fallback can never resurrect an old OTA entry. */
  EraseOtadata();
  g_nvs_ok = (nvs_flash_init_partition("mesh_nvs") == ESP_OK);

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
  int last = LoadLastSlot();
  if (last >= 0 && SlotBootable(last)) {
    ShowAutoBootSplash(last);
  }
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
