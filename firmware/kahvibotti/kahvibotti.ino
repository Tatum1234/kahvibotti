// Kahvibotti: answers /kahvi in the Telegram group with how much coffee is in the pot.
// XIAO ESP32-C3 + 2 HX711. The coffee logic is kahvi_core.h (tested on PC: test/replay_test.cpp).
// Private settings (Wi-Fi, bot token, passwords) live in secrets.h.
// Admin panel (admin.h): http://emukahvibotti.local. Brew stats (stats_core.h) in LittleFS on the data
// partition of partitions.csv; milestones go to the Telegram group.
//
// Three tasks: the sensor task samples the scale ~1/s and feeds the core; the Telegram task polls
// /kahvi and sends milestones (each HTTPS call can block 1-3 s on the C3); the main loop does Wi-Fi,
// the admin web server and stats, so the panel stays responsive while Telegram waits. Serial still prints the calibration-format lines (replayable in the test),
// plus: k = what /kahvi would answer now, # <label> = mark, nollaa-tunnus = clear the panel admin account,
// mem = heap + stack headroom now (also printed every 10 min), bench [N] = time N brews of stats (scratch file).
// No tare command on purpose: re-zeroing mid-run would look like a lift to the core.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Preferences.h>
#include <HX711.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <sys/time.h>
#include <LittleFS.h>
#include "kahvi_core.h"
#include "stats_core.h"
#include "secrets.h"
#include "wifi_setup.h"  // Wi-Fi + the "EmuKahviBottiHotspot" fallback hotspot
#include "fw_update.h"   // firmware slots: app0 = USB original, app1 = uploaded
#include "telegram_roots.h"

// A Telegram answer (tgRequest). Declared right after the includes: the Arduino build inserts function
// prototypes there, and they need the type.
struct TgResp {
  int status = 0;         // HTTP status, 0 = no answer at all
  String body;            // up to 4 KB
  bool complete = false;  // the whole body arrived
};

HX711 left, right;  // D5/D6 and D7/D10, same as calibration_c3
kahvi::Core core;
SemaphoreHandle_t coreLock;
Preferences prefs;
// TCP connect + socket write timeout 10 s instead of 30 s. setTimeout() can't do it: it sets Stream's own
// _timeout, not NetworkClient's, and the library calls connect() without a timeout.
struct TgClient : WiFiClientSecure { TgClient() { _timeout = 10000; } };
TgClient tls;
UniversalTelegramBot bot(BOT_TOKEN, tls);

time_t brewedEpoch = 0;  // wall-clock time of the last finished brew, 0 = unknown (persisted)
int64_t seenBrewedAt = 0;
float lastL = 0, lastR = 0;  // latest raw sample, for the admin page

// Stats: finished brews wait here (under coreLock) until the main loop has a valid clock to date them and
// saves them. Normally at once; after a power cut that also took the internet down, there's no clock (no
// NTP, no Telegram) until it's back, so several can wait. They are in RAM only: panel restarts wait for them
// (at most 20 min). The automatic low-memory and Telegram-silent restarts don't: those recover a device that
// may never get a clock otherwise, which is worth more than a few unsaved brews.
constexpr int PENDING_MAX = 8;
struct PendingBrew { int64_t ms; uint16_t waterG; };
PendingBrew pending[PENDING_MAX];
int pendingN = 0;
float statTotal = 0;  // cups brewed, all time (cached sum of the stats file)
int statBrews = 0;
// The stats file is in time order (brews are appended as they finish), so the Tilastot page can jump to the
// part it shows. Checked at boot and on every append; a clock step back of more than a day (it never happens
// normally) clears it, and the page then reads the whole file (stats::fillPage).
bool statsOrdered = true;
uint32_t statLastEpoch = 0;
static void noteOrder(uint32_t epoch) {
  if (epoch + stats::PAGE_MARGIN < statLastEpoch) statsOrdered = false;
  if (epoch > statLastEpoch) statLastEpoch = epoch;
}
int msAnnounced = 0;  // cups of the last milestone sent to the group (NVS "stats"/"ms")
const char* STATS_FILE = "/brews.bin";
bool fsOk = false;  // stats storage mounted

// Telegram status + settings for the Huolto page. Written by the Telegram task, read/changed by the web
// page: all under coreLock. The group gets milestone and test messages: NVS "tg"/"group", else
// GROUP_CHAT_ID from secrets.h. (/kahvi is answered in every chat that asks.)
String tgGroup;
int64_t tgLastOk = 0;  // last successful contact with Telegram (getMe / updates / a sent message), ms
String tgBotUser;
String tgPollErr;  // Telegram's error for getUpdates ("" = fine), shown on Huolto
int tgAnswersToday = 0, tgAnswerDay = -1;
bool tgTestPending = false;
String tgTestResult;
int64_t milestoneNextTry = 0;  // after a failed milestone send: not before this (nowMs)

static void loadTgGroup() {
  Preferences p;
  p.begin("tg", true);
  String g = p.getString("group", "");
  p.end();
  tgGroup = g.length() ? g : String(GROUP_CHAT_ID);
}
TaskHandle_t sensorTaskHandle = nullptr, telegramTaskHandle = nullptr;  // for the stack readings on Tila
constexpr uint32_t SENSOR_STACK = 6144, TELEGRAM_STACK = 10240;           // bytes

// Event log for the admin panel's Huolto page: the last EV_MAX decisions, RAM only (under coreLock).
constexpr int EV_MAX = 50;
struct Ev { int64_t ms; const char* what; float a, b; };
Ev evRing[EV_MAX];
int evHead = 0, evCount = 0;
static void logEvent(int64_t t, const char* what, float a, float b) {  // call with coreLock held
  evRing[evHead] = {t, what, a, b};
  evHead = (evHead + 1) % EV_MAX;
  if (evCount < EV_MAX) evCount++;
  Serial.printf(">>> event: %s %.0f %.0f\n", what, a, b);
}

static int64_t nowMs() { return esp_timer_get_time() / 1000; }  // 64-bit, no 71-minute rollover
static bool clockValid() { return time(nullptr) > 1700000000; }

// ---------- replies ----------

static void replyText(char* buf, size_t n) {
  xSemaphoreTake(coreLock, portMAX_DELAY);
  kahvi::Reply r = core.reply(nowMs());
  time_t brewedAt = brewedEpoch;
  xSemaphoreGive(coreLock);
  struct tm brewed, today;
  bool haveTime = brewedAt && clockValid();
  if (haveTime) {
    time_t now = time(nullptr);
    localtime_r(&brewedAt, &brewed);
    localtime_r(&now, &today);
  }
  bool sameDay = haveTime && brewed.tm_yday == today.tm_yday && brewed.tm_year == today.tm_year;
  kahvi::format(r, haveTime ? &brewed : nullptr, sameDay, buf, n);
}

// ---------- stats + milestones ----------

// Mount the stats storage. Format ONLY if it was never used (both LittleFS superblock areas blank):
// LittleFS.begin(true) would also wipe years of stats if a mount ever failed for another reason.
static void statsBegin() {
  fsOk = LittleFS.begin(false);
  if (!fsOk) {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    bool blank = part != nullptr;
    uint8_t buf[64];
    for (uint32_t off : {0u, 4096u}) {
      if (!blank || esp_partition_read(part, off, buf, sizeof buf) != ESP_OK) { blank = false; break; }
      for (uint8_t x : buf) if (x != 0xFF) { blank = false; break; }
    }
    if (blank) fsOk = LittleFS.begin(true);  // first use: create the file system
    if (!fsOk) { Serial.println("!!! stats storage can't be mounted; NOT formatting it (the data may be recoverable)"); return; }
  }
  if (LittleFS.exists("/bench.bin")) LittleFS.remove("/bench.bin");  // left by a "bench" cut short by a reset
  File f = LittleFS.open(STATS_FILE, "r");
  stats::Brew b;
  while (f && f.read((uint8_t*)&b, sizeof b) == sizeof b) {
    statTotal += stats::cups(b);
    statBrews++;
    noteOrder(b.epoch);
  }
  if (f) f.close();
  Preferences p;
  p.begin("stats", true);
  msAnnounced = p.getInt("ms", 0);
  p.end();
  // No stats file = no brews yet (or "Tyhjennä tilastot" was cut short by a power cut after removing it):
  // milestones then count from zero, or they would never be announced again.
  if (!LittleFS.exists(STATS_FILE) && msAnnounced) {
    msAnnounced = 0;
    p.begin("stats", false);
    p.putInt("ms", 0);
    p.end();
  }
  Serial.printf(">>> stats: %.1f cups in %d brews, last milestone sent: %d%s\n", statTotal, statBrews, msAnnounced,
                statsOrdered ? "" : " (file not in time order: Tilastot reads all of it)");
}

// Main loop: write a finished brew once the clock can date it (usually at once; after a boot
// without network it waits for NTP or the first Telegram message).
static void statsFlush() {
  if (!clockValid() || !fsOk) return;
  xSemaphoreTake(coreLock, portMAX_DELAY);
  bool any = pendingN > 0;
  PendingBrew pb = any ? pending[0] : PendingBrew{};  // oldest first: the file stays in time order
  if (any) {
    for (int i = 1; i < pendingN; i++) pending[i - 1] = pending[i];
    pendingN--;
  }
  xSemaphoreGive(coreLock);
  if (!any || pb.waterG == 0) return;
  stats::Brew b = {(uint32_t)(time(nullptr) - (nowMs() - pb.ms) / 1000), pb.waterG, 0};
  File f = LittleFS.open(STATS_FILE, "a");
  size_t before = f ? f.size() : 0;
  // write() only fills stdio's buffer (it says 8 even with the storage full): flush it to flash and check
  // that the file really grew, or a brew on a full file system would be counted but lost at the next boot.
  if (f && f.write((const uint8_t*)&b, sizeof b) == sizeof b && (f.flush(), f.size() == before + sizeof b)) {
    statTotal += stats::cups(b);
    statBrews++;
    noteOrder(b.epoch);
    Serial.printf(">>> stats: brew of %u g saved, total %.1f cups\n", b.waterG, statTotal);
  } else {
    Serial.println("!!! stats: could not save the brew (stats storage full?)");
  }
  if (f) f.close();
}

// One HTTPS request to Telegram, read to the end. All Telegram traffic goes through here (not the
// library's calls): the library waits only 1.5 s for an answer and stops at the first moment the socket is
// empty, so a slow reply looked like a failure (a milestone got posted again and again), and it keeps only
// 1500 characters. HTTP/1.0: no chunked bodies. Stops at Content-Length (or when the server closes, 8 s at
// most), keeps up to 4 KB of the body. Telegram task only (it owns `tls`).
static TgResp tgRequest(const char* method, const String& cmd, const String& json = "") {
  TgResp r;
  esp_task_wdt_reset();  // each request is bounded (connect 10 s, handshake 15 s, reading 8 s); a round isn't
  if (!tls.connected() && !tls.connect("api.telegram.org", 443)) {
    tls.stop();
    return r;
  }
  String req = String(method) + " /" + bot.buildCommand(cmd) + " HTTP/1.0\r\nHost: api.telegram.org\r\n";
  if (json.length()) req += "Content-Type: application/json\r\nContent-Length: " + String(json.length()) + "\r\n";
  req += "\r\n";
  req += json;
  tls.print(req);
  String head;
  head.reserve(512);  // grown in place, not re-allocated byte by byte
  r.body.reserve(1024);
  bool inBody = false;
  long len = -1, got = 0;  // Content-Length: stop there, don't rely on the server closing the connection
  uint8_t buf[256];
  int64_t t0 = nowMs();
  while ((tls.connected() || tls.available()) && nowMs() - t0 < 8000 && !(len >= 0 && got >= len)) {
    int n = tls.available() ? tls.read(buf, sizeof buf) : 0;
    if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    for (int k = 0; k < n; k++) {
      if (inBody) {
        got++;
        if (r.body.length() < 4096) r.body += (char)buf[k];
      } else if (head.length() < 2048) {
        head += (char)buf[k];
        if ((inBody = head.endsWith("\r\n\r\n"))) {
          head.toLowerCase();
          int c = head.indexOf("content-length:");
          if (c >= 0) len = atol(head.c_str() + c + 15);
        }
      }
    }
  }
  r.complete = inBody && (len >= 0 ? got >= len : !tls.connected());  // all of it (or the server closed)
  tls.stop();
  head.toLowerCase();
  if (head.startsWith("http/") && head.length() > 12) r.status = atoi(head.c_str() + 9);  // "http/1.1 200 ok"
  // No NTP (UDP 123 blocked)? Every Telegram answer carries the time: without a clock, brews can't be dated.
  int d = head.indexOf("\r\ndate:");
  if (!clockValid() && d >= 0) {
    int64_t e = stats::httpDate(head.c_str() + d + 7);
    if (e > 1700000000) {
      struct timeval tv = {(time_t)e, 0};
      settimeofday(&tv, nullptr);
      Serial.println(">>> clock set from Telegram's Date header");
    }
  }
  return r;
}

// One sendMessage attempt (never retried here).
static TgResp tgPost(const String& chat, const char* text) {
  JsonDocument req;
  req["chat_id"] = chat;
  req["text"] = text;
  String json;
  serializeJson(req, json);
  return tgRequest("POST", "sendMessage", json);
}

// Send to the configured group and return Telegram's own reason on failure, in Finnish. When Telegram says
// the group was upgraded to a supergroup (it then gets a new "-100..." ID), switch to the new ID at once.
// Telegram task only (it owns the TLS connection).
// status (optional) gets the HTTP status: 0 = no answer, so it may or may not have been sent.
static bool sendToGroup(const String& text, String& why, int* status = nullptr) {
  xSemaphoreTake(coreLock, portMAX_DELAY);
  String group = tgGroup;
  xSemaphoreGive(coreLock);
  TgResp r = tgPost(group, text.c_str());
  if (status) *status = r.status;
  if (r.status == 200) { why = ""; return true; }  // Telegram answers 200 only for a sent message
  if (r.status == 0) { why = "ei yhteyttä Telegramiin"; return false; }
  JsonDocument res;
  bool parsed = !deserializeJson(res, r.body);
  String d = parsed ? String(res["description"] | "") : r.body.substring(0, 80);
  String migrate = parsed ? res["parameters"]["migrate_to_chat_id"].as<String>() : "";
  if (migrate.length() && migrate != "null") {
    if (status) *status = 0;  // not a refusal: the group is fixed now, so a milestone retries in a minute
    Preferences p;
    p.begin("tg", false);
    p.putString("group", migrate);
    p.end();
    xSemaphoreTake(coreLock, portMAX_DELAY);
    loadTgGroup();
    logEvent(nowMs(), "Telegram-ryhmä sai uuden tunnuksen (supergroup)", 0, 0);
    xSemaphoreGive(coreLock);
    why = "ryhmä on muutettu isommaksi ryhmäksi (supergroup) ja sai uuden tunnuksen " + migrate +
          ", joka otettiin käyttöön. Lähetä testiviesti uudelleen.";
    return false;
  }
  if (d.indexOf("chat not found") >= 0) why = "ryhmää ei löydy: tunnus on väärä, tai botti ei ole ryhmän jäsen";
  else if (d.indexOf("not a member") >= 0) why = "botti ei ole ryhmän jäsen – lisää botti ryhmään";
  else if (d.indexOf("kicked") >= 0) why = "botti on poistettu ryhmästä – lisää se takaisin";
  else if (d.indexOf("not enough rights") >= 0) why = "botilla ei ole oikeutta kirjoittaa ryhmään";
  else why = "Telegram vastasi: " + d;
  return false;
}

// Telegram task: announce milestones reached, oldest first, each once. A failed send is tried again in a
// minute, or in 30 min after a definite refusal (wrong group, bot removed: saving a group on Huolto ends
// that wait). A send whose answer never came (status 0) may have gone out: then a repeat is possible.
static void milestones() {
  xSemaphoreTake(coreLock, portMAX_DELAY);
  const stats::Milestone* m = nowMs() >= milestoneNextTry ? stats::due(statTotal, msAnnounced) : nullptr;
  xSemaphoreGive(coreLock);
  if (!m) return;
  String why;
  int status = 0;
  if (!sendToGroup(m->text, why, &status)) {
    bool refused = status >= 400 && status < 500 && status != 429;  // 429 = too many requests: soon again
    xSemaphoreTake(coreLock, portMAX_DELAY);
    milestoneNextTry = nowMs() + (refused ? 30 * 60000 : 60000);
    xSemaphoreGive(coreLock);
    Serial.printf("!!! milestone not sent: %s\n", why.c_str());
    return;
  }
  // Under the lock, NVS included (like saveIfChanged): "Tyhjennä tilastot" does the same, so its reset can't
  // land between this RAM update and this NVS write.
  xSemaphoreTake(coreLock, portMAX_DELAY);
  bool still = statTotal >= m->cups;  // false if the stats were cleared while it was being sent
  if (still) {
    msAnnounced = m->cups;
    Preferences p;
    p.begin("stats", false);
    p.putInt("ms", msAnnounced);
    p.end();
  }
  xSemaphoreGive(coreLock);
  if (!still) return;
  Serial.printf(">>> milestone %d sent\n", m->cups);
}

// ---------- sensor task ----------

// Average of `times` samples, waiting at most 300 ms for each (the HX711 gives 10 a second). The library's
// read()/read_average()/tare() wait *forever* for a chip that never gets ready (a broken wire, a dead
// module): the watchdog would restart the board, and it would hang again in setup() before Wi-Fi, in a
// loop. The 1 ms delay while waiting also lets the lower-priority tasks run (delay(0) busy-waited).
static bool avgRead(HX711& hx, int times, long& out) {
  int64_t sum = 0;
  for (int i = 0; i < times; i++) {
    if (!hx.wait_ready_timeout(300, 1)) return false;
    sum += hx.read();  // ready now: read() doesn't wait
  }
  out = (long)(sum / times);
  return true;
}

// One averaged reading. False if the HX711 doesn't answer, is saturated (a broken wire reads full
// scale), or the value is far outside anything the machine can produce (~±500k).
static bool readCell(HX711& hx, long& out) {
  long raw;
  if (!avgRead(hx, 5, raw)) return false;
  if (raw >= 8388000 || raw <= -8388000) return false;
  out = raw - hx.get_offset();
  return labs(out) < 3000000;
}

// Persist what can't be re-measured after a power cut. Written only when something changes
// (a handful of times a day), so flash wear is a non-issue. Call with coreLock held.
static void saveIfChanged() {
  static bool c = true, b = true, m = false;
  static int pot = 0;
  static time_t br = 0;
  int p = (int)lroundf(core.potG);
  if (c == core.carafeOn && b == core.basketOn && m == core.machineAway && abs(pot - p) < 5 && br == brewedEpoch) return;
  c = core.carafeOn; b = core.basketOn; m = core.machineAway; pot = p; br = brewedEpoch;
  prefs.putBool("machine", m);
  prefs.putBool("carafe", c);
  prefs.putBool("basket", b);
  prefs.putInt("pot", pot);
  prefs.putLong64("brewed", br);
}

static void sensorTask(void*) {
  esp_task_wdt_add(NULL);
  char last[160] = "";
  for (;;) {
    long l, r;
    if (readCell(left, l) && readCell(right, r)) {
      xSemaphoreTake(coreLock, portMAX_DELAY);
      core.feed(nowMs(), l, r);
      lastL = l; lastR = r;
      if (core.brewedAt != seenBrewedAt) {  // a brew just finished, or 0: machine came back, fresh start
        seenBrewedAt = core.brewedAt;
        brewedEpoch = core.brewedAt && clockValid() ? time(nullptr) - (nowMs() - core.brewedAt) / 1000 : 0;
        if (core.brewedAt && fsOk && core.brewWaterG >= 1) {  // nothing to wait for without stats storage
          if (pendingN == PENDING_MAX) {  // 8 brews without a clock: keep the newest
            for (int i = 1; i < PENDING_MAX; i++) pending[i - 1] = pending[i];
            pendingN--;
          }
          pending[pendingN++] = {core.brewedAt, (uint16_t)lroundf(core.brewWaterG)};
        }
      }
      // A fresh start (panel reset, the machine put back) with no brew since boot leaves brewedAt at 0, so
      // the check above doesn't see it: forget the brew time restored at boot too.
      static uint32_t seenFresh = 0;
      if (core.freshStarts != seenFresh) {
        seenFresh = core.freshStarts;
        brewedEpoch = 0;
      }
      // A brew that finished before there was a clock gets its time once the clock is known.
      if (!brewedEpoch && core.brewedAt && clockValid()) brewedEpoch = time(nullptr) - (nowMs() - core.brewedAt) / 1000;
      saveIfChanged();
      xSemaphoreGive(coreLock);
      Serial.printf("t=%10llu us  left : %8ld raw   right: %8ld raw    rawT=%9ld\n", esp_timer_get_time(), l, r, l + r);
    } else {
      // skipped: the core says "Vaaka ei vastaa" after 30 s without good samples
      Serial.println("!!! bad or missing HX711 reading, sample skipped");
    }
    char buf[160];
    replyText(buf, sizeof buf);
    if (strcmp(buf, last)) { strcpy(last, buf); Serial.printf(">>> state: %s\n", buf); }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(150));  // ~1.15 s per sample, the pace the core was tuned on
  }
}

#include "admin.h"  // web pages; uses the globals and helpers above

// ---------- Telegram ----------

static bool isKahviCommand(const String& text) {  // "/kahvi", "/kahvi@thisbot", "/kahvi jotain"
  if (!text.startsWith("/kahvi")) return false;
  if (text.length() == 6 || text[6] == ' ') return true;
  if (text[6] != '@') return false;
  xSemaphoreTake(coreLock, portMAX_DELAY);
  String me = tgBotUser;
  xSemaphoreGive(coreLock);
  int end = text.indexOf(' ');
  String to = text.substring(7, end < 0 ? text.length() : end);
  return !me.length() || to.equalsIgnoreCase(me);  // "/kahvi@SomeOtherBot" is for another bot
}

static void noteKahvi() {  // count today's /kahvi answers for the Huolto page; call with coreLock held
  if (clockValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_yday != tgAnswerDay) { tgAnswerDay = t.tm_yday; tgAnswersToday = 0; }
  }
  tgAnswersToday++;
}

// Fetch the next update ourselves instead of bot.getUpdates(). The library keeps only the first 1500
// characters of a response and stops reading as soon as the socket is momentarily empty. An update that
// doesn't fit (a longer Finnish message: Telegram sends ä as "\u00e4"; a photo, a reply, a forward, ...)
// then fails to parse, the offset never moves past it, and every later /kahvi is stuck behind it until
// Telegram drops it after 24 h (a reboot doesn't help). Here: read to the end (tgRequest, up to 4 KB) and
// skip any update that still doesn't parse, so the offset always moves on.
// Returns -1 = no/failed contact, 0 = nothing new, 1 = one update (chat/text/date empty if not a message).
static long long tgOffset = 0;
static int tgNextUpdate(String& chat, String& text, int64_t& date) {
  chat = ""; text = ""; date = 0;
  TgResp r = tgRequest("GET", "getUpdates?limit=1&offset=" + String(tgOffset));
  if (!r.body.length()) return -1;
  JsonDocument filter;
  filter["ok"] = true;
  filter["description"] = true;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["message"]["text"] = true;
  filter["result"][0]["message"]["date"] = true;
  filter["result"][0]["message"]["chat"]["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, r.body, DeserializationOption::Filter(filter))) {
    // Too big even for 4 KB: skip it by its id. A body cut short by a slow link is just retried.
    int at = r.body.indexOf("\"update_id\":");
    if (at < 0 || (!r.complete && r.body.length() < 4096)) return -1;
    tgOffset = atoll(r.body.c_str() + at + 12) + 1;
    Serial.printf(">>> telegram: update too long (%u+ chars) skipped\n", r.body.length());
    return 1;
  }
  if (doc["ok"] != true) {  // e.g. "Conflict: terminated by other getUpdates request" (the same token in use elsewhere)
    String d = doc["description"] | "tuntematon virhe";
    Serial.printf(">>> telegram getUpdates: %s\n", d.c_str());
    xSemaphoreTake(coreLock, portMAX_DELAY);
    tgPollErr = d;
    xSemaphoreGive(coreLock);
    return -1;
  }
  xSemaphoreTake(coreLock, portMAX_DELAY);
  tgPollErr = "";
  xSemaphoreGive(coreLock);
  JsonArray res = doc["result"];
  if (res.size() == 0) return 0;
  tgOffset = res[0]["update_id"].as<long long>() + 1;
  JsonObject m = res[0]["message"];
  if (!m.isNull()) {
    chat = m["chat"]["id"].as<String>();
    text = m["text"] | "";
    date = m["date"] | (int64_t)0;  // 64-bit: a 32-bit long would break in 2038
  }
  return 1;
}

static void handleTelegram() {
  static bool drained = false;
  static kahvi::ReplyLimiter limiter;
  // After boot Telegram still holds up to 24 h of queued updates: offset -1 fetches only the newest one and
  // drops the rest, and that one is skipped too, so nothing stale gets answered.
  if (!drained) tgOffset = -1;
  // With privacy mode off every group message arrives, so empty the queue each round (up to 20), or a /kahvi
  // could wait behind a burst of chatter until it's too old to answer.
  for (int round = 0; round < 20; round++) {
    String chat, text;
    int64_t date;
    int r = tgNextUpdate(chat, text, date);
    if (r < 0) return;
    xSemaphoreTake(coreLock, portMAX_DELAY);
    tgLastOk = nowMs();
    xSemaphoreGive(coreLock);
    if (!drained) {
      drained = true;
      if (tgOffset < 0) tgOffset = 0;  // the queue was empty: back to normal offsets
      continue;
    }
    if (r == 0) return;
    time_t sent = (time_t)date;
    if (!chat.length() || !isKahviCommand(text)) continue;  // privacy mode is off, so all group chatter arrives
    if (clockValid() && sent > 0 && time(nullptr) - sent > 120) continue;  // stale (e.g. Wi-Fi was down)
    // Spam guard on the time each message was *sent*, not when it's handled: handling takes 1-3 s per
    // message (a TLS connection each), so 20 quick /kahvi would otherwise spread over a minute and many
    // would pass the 10 s gap.
    if (!limiter.allow(strtoll(chat.c_str(), nullptr, 10), (int64_t)sent * 1000)) continue;
    char buf[160];
    replyText(buf, sizeof buf);
    tgPost(chat, buf);  // one attempt: the library's sendMessage retries for 8 s on any error
    xSemaphoreTake(coreLock, portMAX_DELAY);
    noteKahvi();
    xSemaphoreGive(coreLock);
    Serial.printf(">>> /kahvi from %s: %s\n", chat.c_str(), buf);
  }
}

// "Lähetä testiviesti" on the Huolto page only sets a flag: this task owns the Telegram connection.
static void telegramHousekeeping() {
  static int64_t lastMe = -300000;
  if (nowMs() - lastMe > 300000) {  // every 5 min: are we reaching Telegram, and what's the bot's name
    lastMe = nowMs();
    TgResp r = tgRequest("GET", "getMe");
    JsonDocument filter, me;
    filter["result"]["username"] = true;
    if (r.status == 200 && !deserializeJson(me, r.body, DeserializationOption::Filter(filter))) {
      String user = me["result"]["username"] | "";
      xSemaphoreTake(coreLock, portMAX_DELAY);
      tgLastOk = nowMs();
      if (user.length()) tgBotUser = user;
      xSemaphoreGive(coreLock);
    }
  }
  xSemaphoreTake(coreLock, portMAX_DELAY);
  bool test = tgTestPending;
  String group = tgGroup;
  xSemaphoreGive(coreLock);
  if (!test) return;
  String why;
  bool ok = sendToGroup("☕ Kahvibotin testiviesti: yhteys tähän ryhmään toimii.", why);
  xSemaphoreTake(coreLock, portMAX_DELAY);
  tgTestPending = false;
  if (ok) tgLastOk = nowMs();
  tgTestResult = ok ? "✓ Testiviesti lähetetty ryhmään " + group + "."
                    : "⚠ Testiviestiä ei voitu lähettää ryhmään " + group + ": " + why + ".";
  xSemaphoreGive(coreLock);
}

volatile uint32_t wifiEpoch = 0;  // bumped on every (re)connect: the Telegram task then drops its TLS socket

static void telegramTask(void*) {
  esp_task_wdt_add(NULL);
  uint32_t seen = 0;
  for (;;) {
    esp_task_wdt_reset();
    if (WiFi.status() == WL_CONNECTED) {
      if (seen != wifiEpoch) { tls.stop(); seen = wifiEpoch; }  // never reuse a socket from before a drop
      handleTelegram();  // every Telegram request also resets the watchdog (tgRequest)
      esp_task_wdt_reset();
      milestones();
      esp_task_wdt_reset();
      telegramHousekeeping();
    }
    vTaskDelay(pdMS_TO_TICKS(2000));  // /kahvi answered within ~2-4 s
  }
}

// ---------- long-term upkeep ----------
static void printMem() {  // serial "mem", and every 10 min. Main loop only (its own stack is "loop")
  Serial.printf(">>> mem: up %lld min, free %u, min %u, block %u, stack left sensors %u telegram %u loop %u\n",
                nowMs() / 60000, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(sensorTaskHandle),
                (unsigned)uxTaskGetStackHighWaterMark(telegramTaskHandle), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// Weekly restart (Sunday 04:00 local, after at least a day up) keeps heap fragmentation from TLS and web
// traffic from building up over months; the state is in NVS, so nothing is lost. And a safety restart
// if memory runs low for 5 minutes in a row (TLS needs ~16 KB contiguous). Neither happens mid-brew.
static void upkeep() {
  static int64_t last = 0;
  static int low = 0;
  if (nowMs() - last < 60000 || admin::restartAt) return;
  last = nowMs();
  xSemaphoreTake(coreLock, portMAX_DELAY);
  bool idle = !core.brewing && !core.flowing;
  xSemaphoreGive(coreLock);
  size_t freeHeap = ESP.getFreeHeap(), block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  low = (freeHeap < 30000 || block < 16000) ? low + 1 : 0;
  static int64_t lastMem = -600000;
  if (nowMs() - lastMem >= 600000) { lastMem = nowMs(); printMem(); }  // for long-run leak checks over USB
  uint8_t why = 0;
  if (low >= 5 && idle) why = fw::PLANNED_LOWMEM;
  // Wi-Fi up, but no contact with Telegram (a stalled router, DNS or network stack): nothing else notices,
  // and the bot would be silent until the Sunday restart. Counted in minutes *online* (a Wi-Fi outage isn't
  // Telegram's fault, and the clock mustn't fire the moment Wi-Fi returns): at 2 h and 4 h reconnect Wi-Fi,
  // at 6 h restart. If Telegram is blocked for good, that's a restart every 6 h: harmless.
  static int silentMin = 0;
  xSemaphoreTake(coreLock, portMAX_DELAY);
  int64_t okAt = tgLastOk;  // 0 = not once since boot; normally renewed every few seconds
  xSemaphoreGive(coreLock);
  if (WiFi.status() == WL_CONNECTED) {  // offline minutes neither count nor act (a kick mid-reconnect would abort it)
    silentMin = okAt && nowMs() - okAt < 10 * 60000 ? 0 : silentMin + 1;
    if (silentMin >= 360) {
      if (idle && !why) why = fw::PLANNED_TELEGRAM;
    } else if (silentMin && silentMin % 120 == 0) {
      xSemaphoreTake(coreLock, portMAX_DELAY);
      logEvent(nowMs(), "Telegram ei vastannut (h): Wi-Fi yhdistetään uudelleen", silentMin / 60, 0);
      xSemaphoreGive(coreLock);
      WiFi.disconnect();  // wifisetup::loop() reconnects to the known networks
    }
  }
  if (clockValid() && idle && esp_timer_get_time() > 24LL * 3600 * 1000000) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_wday == 0 && t.tm_hour == 4 && t.tm_min < 5) why = fw::PLANNED_WEEKLY;
  }
  if (!why) return;
  xSemaphoreTake(coreLock, portMAX_DELAY);
  logEvent(nowMs(), why == fw::PLANNED_WEEKLY     ? "Viikkohuolto: uudelleenkäynnistys"
                    : why == fw::PLANNED_TELEGRAM ? "Telegram ei vastannut 6 h: uudelleenkäynnistys"
                                                  : "Muisti vähissä: uudelleenkäynnistys",
           freeHeap, block);
  xSemaphoreGive(coreLock);
  admin::restartWhy = why;
  admin::restartAt = nowMs() + 1000;  // the same path as a panel restart (marks the running firmware valid)
}

// ---------- setup / loop ----------

// Arduino core hook: don't mark a freshly uploaded firmware valid at boot. fw::loop() does it after 3
// minutes up, so a firmware that crashes before then is rolled back by the bootloader (fw_update.h).
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on logging: a plugged-in PC that isn't reading would stall
                             // every Serial.print up to ~2 s (HWCDC backpressure path)
  delay(2000);
  fw::bootCheck();  // may switch back to the original firmware and restart

  // Watchdog: any task stuck for 60 s (a dead HX711 mid-read, a network call that never returns)
  // restarts the board instead of leaving it silent.
  esp_task_wdt_config_t wdt = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(NULL);

  // Restore the last known state (power cut). If things changed while it was off, the next
  // carafe event re-measures and self-heals.
  prefs.begin("kahvi", false);
  core.machineAway = prefs.getBool("machine", false);
  core.carafeOn = prefs.getBool("carafe", true);
  core.basketOn = prefs.getBool("basket", true);
  core.potG = prefs.getInt("pot", 0);
  brewedEpoch = (time_t)prefs.getLong64("brewed", 0);
  Serial.printf("Restored: machine %s, carafe %s, basket %s, pot %.0f g\n", core.machineAway ? "AWAY" : "on",
                core.carafeOn ? "on" : "off",
                core.basketOn ? "on" : "off", core.potG);

  left.begin(7, 21);
  right.begin(20, 10);
  long zero;  // readability only: the core works on steps, the zero doesn't matter
  if (avgRead(left, 20, zero)) left.set_offset(zero);
  else Serial.println("!!! left HX711 not answering at start");
  if (avgRead(right, 20, zero)) right.set_offset(zero);
  else Serial.println("!!! right HX711 not answering at start");

  coreLock = xSemaphoreCreateMutex();
  core.onEvent = logEvent;  // runs inside core.feed(), i.e. with coreLock held
  static char bootText[96];  // the event log keeps a pointer: must outlive setup()
  snprintf(bootText, sizeof bootText, "Käynnistys (%s versio, syy: %s)", fw::runningUpdate() ? "päivitetty" : "alkuperäinen",
           fw::resetText(esp_reset_reason()));
  logEvent(nowMs(), bootText, 0, 0);
  Serial.printf(">>> %s\n", bootText);
  if (fw::fellBack) logEvent(nowMs(), "Päivitetty versio kaatui – palattu alkuperäiseen", 0, 0);
  statsBegin();
  loadTgGroup();
  xTaskCreate(sensorTask, "sensors", SENSOR_STACK, nullptr, 2, &sensorTaskHandle);

  wifisetup::begin();
  admin::begin();
  xTaskCreate(telegramTask, "telegram", TELEGRAM_STACK, nullptr, 1, &telegramTaskHandle);
  tls.setCACert(TELEGRAM_ROOTS);  // Go Daddy Root G2 (Telegram's today) + 3 other common roots
  tls.setHandshakeTimeout(15);  // s (default 120 s: a stalled handshake would outlast the 60 s watchdog)
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");  // Finland
  Serial.println("Kahvibotti running. Serial: k = /kahvi answer, # <label> = mark");
}

String inputBuffer;

void loop() {
  esp_task_wdt_reset();

  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') { if (c != '\r' && inputBuffer.length() < 64) inputBuffer += c; continue; }
    inputBuffer.trim();
    if (inputBuffer == "k") {
      char buf[160];
      replyText(buf, sizeof buf);
      Serial.printf(">>> /kahvi: %s\n", buf);
    } else if (inputBuffer == "nollaa-tunnus") {  // physical access only: forgotten panel admin account
      Preferences p;
      p.begin("admin", false);
      p.clear();
      p.end();
      admin::loadPanelAccount();
      Serial.println(">>> panel admin account cleared: the owner's login (secrets.h) now also opens the hotspot");
    } else if (inputBuffer == "mem") {
      printMem();
    } else if (inputBuffer.startsWith("bench")) {  // "bench 40000": time 10 years of stats on a scratch file
      admin::statsBench(inputBuffer.length() > 6 ? inputBuffer.substring(6).toInt() : 40000);
    } else if (inputBuffer.startsWith("#")) {
      String label = inputBuffer.substring(1);
      label.trim();
      Serial.printf("\n############ MARK t=%10llu us: %s ############\n\n", esp_timer_get_time(), label.c_str());
    }
    inputBuffer = "";
  }

  static bool wasConnected = false;
  bool connected = wifisetup::loop();
  if (connected != wasConnected) {
    if (connected) {
      Serial.printf(">>> Wi-Fi connected to '%s', IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      wifiEpoch++;
    }
    else Serial.println(">>> Wi-Fi lost");
    wasConnected = connected;
  }
  admin::loop();
  xSemaphoreTake(coreLock, portMAX_DELAY);
  int64_t lastFeed = core.lastFeed;  // 64-bit: read whole, not half before and half after a sensor update
  xSemaphoreGive(coreLock);
  fw::loop(connected && nowMs() - lastFeed < 5000);  // healthy = online + sensor data flowing
  statsFlush();
  upkeep();
  delay(10);
}
