// Firmware slots (partitions.csv): app0 = the USB-flashed "alkuperäinen" firmware, app1 = the one
// firmware uploaded from the admin panel.
//
// Protection: the web updater only ever writes app1, and only while app0 is running (Update.begin()
// writes "the next OTA slot" and ignores its label argument, so we check that the next slot really
// is app1). A USB flash always boots app0 (the upload writes boot_app0.bin, which selects the first OTA
// slot), so the original can't be lost from the browser.
// Fallback, two layers:
// 1. The bootloader's app rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in core 3.3.11). A freshly
//    uploaded image is "pending verify" until marked valid. The core would mark it at once, but
//    kahvibotti.ino overrides verifyRollbackLater(), so fw::loop() marks it only once it's proven
//    itself: online + sensor data flowing for 20 s (or 3 minutes up regardless). Any restart before
//    that, even a crash before setup(), makes the bootloader boot app0 again. At that boot the broken
//    image is deleted, once, and the reason it was reset is kept for the panel.
// 2. Later on (image already valid): 3 restarts each within 3 minutes of boot -> back to app0.
#pragma once
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <Preferences.h>
#include <esp_core_dump.h>

namespace fw {

inline const esp_partition_t* app(int i) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  i == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}

inline bool runningUpdate() { return esp_ota_get_running_partition() == app(1); }

// "Delete" a slot or the crash dump by erasing only its first 4 KB sector (~45 ms): that's where the image
// header (and the app description) or the core dump header live, so the slot reads as empty and can't
// boot. Erasing the whole 1.5 MB / 64 KB region in one call used 64 KB block erases, which can outlast
// the 300 ms interrupt watchdog: in the 2026-09-25 hardware test, "Kuittaa" and "Tyhjennä kaatumistieto"
// both reset the board. A new upload rewrites the slot sector by sector anyway.
inline bool eraseHeader(const esp_partition_t* part) {
  return part && esp_partition_erase_range(part, 0, 4096) == ESP_OK;
}

// The uploads go here, so it must be app1, and only when app0 is running.
inline bool canUpload() { return !runningUpdate() && esp_ota_get_next_update_partition(nullptr) == app(1); }

inline bool hasFirmware(int i) {
  esp_app_desc_t d;
  return app(i) && esp_ota_get_partition_description(app(i), &d) == ESP_OK;
}

// "25.9.2026 14:02" style build stamp of the firmware in a slot, or "tyhjä".
inline String built(int i) {
  esp_app_desc_t d;
  if (!app(i) || esp_ota_get_partition_description(app(i), &d) != ESP_OK) return "tyhjä";
  return String(d.date) + " " + String(d.time);
}

bool fellBack = false;  // set at boot if an uploaded firmware was abandoned
int fellBackReason = 0;  // esp_reset_reason_t that ended it (read at the rollback boot)

inline const char* resetText(int r) {
  switch (r) {
    case ESP_RST_POWERON: return "virta kytketty";
    case ESP_RST_EXT: return "ulkoinen nollaus";
    case ESP_RST_SW: return "ohjelmiston uudelleenkäynnistys";
    case ESP_RST_PANIC: return "ohjelmavirhe (kaatuminen)";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "vahtikoira (jumiutui)";
    case ESP_RST_BROWNOUT: return "jännite notkahti";
    case ESP_RST_USB: return "USB-sarjaportin avaus";
    case ESP_RST_JTAG: return "JTAG";
    default: return "tuntematon";
  }
}

// Boot history (NVS "boots2", last 5): which version started and why the previous run ended. It
// survives resets and rollbacks, so a failed update can be diagnosed without a serial monitor.
// Entry (16 bits, NVS "boots2"): bit 15 = updated version, bits 8-14 = planned restart by the bot (PLANNED_*),
// bits 0-7 = reset reason. (The old 8-bit "boots" had room for only 4 planned reasons.)
constexpr int HIST = 5;
constexpr uint8_t PLANNED_NONE = 0, PLANNED_WEEKLY = 1, PLANNED_LOWMEM = 2, PLANNED_PANEL = 3, PLANNED_TELEGRAM = 4;
uint16_t hist[HIST];
int histN = 0;

// Before a restart the bot decides on itself: remembered so the history can say why.
inline void notePlanned(uint8_t why) {
  Preferences p;
  p.begin("fw", false);
  p.putUChar("planned", why);
  p.end();
}

inline const char* plannedText(int why) {
  return why == PLANNED_WEEKLY ? " (viikkohuolto)" : why == PLANNED_LOWMEM ? " (muisti vähissä)" :
         why == PLANNED_PANEL ? " (hallintapaneelista)" : why == PLANNED_TELEGRAM ? " (Telegram ei vastannut 6 h)" : "";
}

// First thing in setup().
inline void bootCheck() {
  Preferences p;
  p.begin("fw", false);
  if (p.isKey("hist")) p.remove("hist");    // the old 8-entry format
  if (p.isKey("boots")) p.remove("boots");  // the old 8-bit format (history starts over once)
  histN = p.getBytes("boots2", hist, sizeof hist) / sizeof hist[0];
  for (int i = HIST - 1; i > 0; i--) hist[i] = hist[i - 1];  // newest first
  uint8_t planned = p.getUChar("planned", PLANNED_NONE) & 0x7f;
  p.putUChar("planned", PLANNED_NONE);
  hist[0] = (runningUpdate() ? 0x8000 : 0) | (planned << 8) | ((int)esp_reset_reason() & 0xff);
  if (histN < HIST) histN++;
  p.putBytes("boots2", hist, histN * sizeof hist[0]);  // only the real entries (no blank rows after a reset)
  if (!runningUpdate()) {
    esp_ota_img_states_t st;
    // The bootloader rolled back. otadata keeps saying so even after the slot is erased, so only act
    // while the broken image is still there: exactly once per failed update.
    if (app(1) && esp_ota_get_state_partition(app(1), &st) == ESP_OK &&
        (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID) && hasFirmware(1)) {
      eraseHeader(app(1));  // a broken update is of no use: delete it
      p.putBool("fellback", true);
      p.putInt("fbreason", (int)esp_reset_reason());  // why the updated version was reset
      Serial.printf("!!! uploaded firmware was reset before proving itself (%s): rolled back, update deleted\n",
                    resetText(esp_reset_reason()));
    }
    if (p.getBool("erase1", false)) {  // "Palaa alkuperäiseen ja poista päivitys" from app1
      eraseHeader(app(1));
      p.putBool("erase1", false);
      Serial.println(">>> uploaded firmware deleted");
    }
    // Told to boot app1, but the bootloader couldn't load it and started app0: point otadata back to
    // app0 (so every boot doesn't try the dead slot first) and report it like a failed update.
    if (esp_ota_get_boot_partition() == app(1)) {
      esp_ota_set_boot_partition(app(0));
      p.putBool("fellback", true);
      p.putInt("fbreason", (int)esp_reset_reason());
      Serial.println("!!! the uploaded firmware could not be started: back to the original");
    }
    p.putInt("fails", 0);
    fellBack = p.getBool("fellback", false);
    fellBackReason = p.getInt("fbreason", 0);
    // A failed update that's still stored (the 3-restarts path below can't erase its own slot, and
    // older firmware didn't always): delete it now.
    if (fellBack && hasFirmware(1)) {
      eraseHeader(app(1));
      Serial.println(">>> the failed update was deleted");
    }
  } else {
    // Count only runs of this version that ended in a crash (panic or a watchdog), not power cuts or
    // planned restarts: three power flickers soon after boot must not throw away a working update.
    esp_reset_reason_t why = esp_reset_reason();
    bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
                   why == ESP_RST_CPU_LOCKUP;
    int fails = p.getInt("fails", 0) + (crashed ? 1 : 0);
    if (crashed) p.putInt("fails", fails);
    if (fails >= 3 && esp_ota_set_boot_partition(app(0)) == ESP_OK) {  // (if that fails, keep running this one)
      p.putInt("fails", 0);
      p.putBool("fellback", true);
      p.putInt("fbreason", (int)esp_reset_reason());
      p.putBool("erase1", true);  // app0 deletes this failed slot at its next boot
      p.end();
      Serial.println("!!! updated firmware crashed 3 times without settling: back to the original");
      ESP.restart();
    }
  }
  p.end();
}

// The running firmware works: cancel the bootloader rollback (no-op if already valid).
inline void markValid() { esp_ota_mark_app_valid_cancel_rollback(); }

// Main loop. The running firmware has proven itself: healthy (online, sensors) for 20 s up, or 3 min.
inline void loop(bool healthy) {
  static bool done = false;
  int64_t up = esp_timer_get_time();
  if (done || !((healthy && up > 20LL * 1000000) || up > 180LL * 1000000)) return;
  done = true;
  markValid();
  Preferences p;
  p.begin("fw", false);
  p.putInt("fails", 0);
  p.end();
}

// Clear the "update failed" notice only. Used after a new upload: must NOT touch app1, which now holds
// the fresh firmware (the 2026-09-25 bug: it erased the upload it was meant to follow).
inline void clearFailureFlag() {
  Preferences p;
  p.begin("fw", false);
  p.putBool("fellback", false);
  p.end();
  fellBack = false;
}

// "Kuittaa": acknowledge a failed update and delete it if it's still stored (only while app0 runs).
inline void ackFailure() {
  clearFailureFlag();
  if (!runningUpdate() && hasFirmware(1)) eraseHeader(app(1));
}

// The last crash saved to flash (the core writes a core dump on a panic), as text, or "" if none.
// The PC/RA addresses can be looked up in the matching build's .elf with addr2line.
inline String crashInfo() {
  if (esp_core_dump_image_check() != ESP_OK) return "";
  esp_core_dump_summary_t* sm = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
  if (!sm) return "";
  String out;
  if (esp_core_dump_get_summary(sm) == ESP_OK) {
    char b[200], sha[9];
    memcpy(sha, sm->app_elf_sha256, 8);
    sha[8] = 0;
    snprintf(b, sizeof b, "tehtävä %s, PC 0x%08lx, RA 0x%08lx, mcause %lu, mtval 0x%08lx, ohjelmisto %s",
             sm->exc_task, (unsigned long)sm->exc_pc, (unsigned long)sm->ex_info.ra,
             (unsigned long)sm->ex_info.mcause, (unsigned long)sm->ex_info.mtval, sha);
    out = b;
  }
  free(sm);
  return out;
}

inline void clearCrash() {
  eraseHeader(esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr));
}

// Boot the original next time; optionally delete the uploaded firmware (done at that boot, since a
// running slot can't be erased).
inline bool bootOriginal(bool deleteUpdate) {  // false: the boot slot couldn't be set (nothing changes)
  if (esp_ota_set_boot_partition(app(0)) != ESP_OK) return false;
  Preferences p;
  p.begin("fw", false);
  if (deleteUpdate) p.putBool("erase1", true);
  p.end();
  return true;
}

inline bool bootUpdate() { return hasFirmware(1) && esp_ota_set_boot_partition(app(1)) == ESP_OK; }

inline bool deleteUpdateNow() {  // only while the original runs
  return !runningUpdate() && eraseHeader(app(1));
}

}  // namespace fw
