// Admin panel: http://emukahvibotti.local (or the IP), on the office network or the setup hotspot.
// Tila + Tilastot are public (everyone on the network, rate-limited); the rest needs a login (/kirjaudu,
// session cookie; two accounts, see below), except the Wi-Fi page reached through the setup hotspot:
// the hotspot itself is WPA2-protected, and phones' captive-portal popups can't show a login page.
// Plain HTTP on the LAN; HTTPS is too heavy for the C3.
//
// Included from kahvibotti.ino after the globals it uses (core, coreLock, replyText, stats cache).
// Pages: Tila (/), Tilastot (/tilastot, month calendar or ?v=vuosi, /tilastot.csv), Huolto (/huolto:
// live readings, event log, reset / restart / clear stats), Wi-Fi (/wifi), Ohjelmisto (/ohjelmisto:
// firmware upload to app1, switch, delete; see fw_update.h).
#pragma once
#ifndef ADMIN_USER
#define ADMIN_USER "admin"  // older secrets.h without the line
#endif
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <Update.h>

namespace admin {

WebServer server(80);

inline bool viaHotspot() { return wifisetup::portalOn && server.client().localIP() == WiFi.softAPIP(); }

// Two admin accounts, either can log in:
// - the owner's (ADMIN_USER / ADMIN_PASSWORD in secrets.h): fixed, can't be changed or removed from the
//   panel, the permanent fallback;
// - the panel admin's (NVS "admin"), set and changed on the Huolto page. Once set, its password is also
//   the "EmuKahviBottiHotspot" hotspot password (see wifisetup::portalPass()), since the office admins need
//   the hotspot when the Wi-Fi breaks. Forgotten while offline: type `nollaa-tunnus` on the USB serial
//   console (physical access) and the owner's password applies again everywhere.
String panelUser, panelPass;

inline void loadPanelAccount() {
  Preferences p;
  p.begin("admin", true);
  panelUser = p.getString("user", "");
  panelPass = p.getString("pass", "");
  p.end();
}

// Login = a session cookie "kb" (32 random hex chars from the hardware RNG), not browser Basic auth: that
// one can't log out (the browser keeps re-sending the password). Sessions live in RAM (a restart logs
// everyone out), expire after 12 h idle, cookie HttpOnly + SameSite=Lax (other sites cannot POST with it; the actions are all POST). Plain
// HTTP on the office LAN, as before.
constexpr int SESSIONS = 6;
constexpr int64_t SESSION_IDLE_MS = 12LL * 3600 * 1000;
struct Session { String tok; int64_t seen = 0; bool owner = false; };
Session sessions[SESSIONS];

inline String cookieToken() {
  String c = server.header("Cookie");
  int i = c.indexOf("kb=");
  if (i < 0) return "";
  int e = c.indexOf(';', i);
  return c.substring(i + 3, e < 0 ? c.length() : e);
}

inline Session* currentSession() {
  String t = cookieToken();
  if (t.length() != 32) return nullptr;
  for (Session& x : sessions) {
    if (!x.tok.length() || x.tok != t) continue;
    if (nowMs() - x.seen > SESSION_IDLE_MS) { x.tok = ""; return nullptr; }
    x.seen = nowMs();
    return &x;
  }
  return nullptr;
}

inline bool loggedIn() { return currentSession() != nullptr; }
inline bool isOwner() { Session* x = currentSession(); return x && x->owner; }

inline String newSession(bool owner) {
  char b[33];
  for (int i = 0; i < 4; i++) snprintf(b + i * 8, 9, "%08lx", (unsigned long)esp_random());
  Session* slot = &sessions[0];  // a free slot, else the longest-idle one
  for (Session& x : sessions) {
    if (!x.tok.length()) { slot = &x; break; }
    if (x.seen < slot->seen) slot = &x;
  }
  slot->tok = b;
  slot->seen = nowMs();
  slot->owner = owner;
  return slot->tok;
}

// Admin-only pages start with this: logged in (or the Wi-Fi page reached through the setup hotspot,
// which is already protected by its own password), else off to the login page.
inline bool admin(bool wifiPage = false) {
  if (wifiPage && viaHotspot()) return true;
  if (loggedIn()) return true;
  server.sendHeader("Location", "/kirjaudu?next=" + server.uri());
  server.send(303);
  return false;
}

// Public pages (Tila, Tilastot, CSV) start with this: at most 10 requests per 2 s from everyone together
// (admins exempt), so nobody can keep the device busy. It serves one request at a time anyway, and the
// scale and Telegram run in their own tasks, so heavy web use can only slow pages down, never the bot.
inline bool publicOk() {
  static int64_t windowStart = 0;
  static int count = 0;
  if (nowMs() - windowStart > 2000) { windowStart = nowMs(); count = 0; }
  if (++count <= 10 || loggedIn()) return true;
  server.send(503, "text/plain; charset=utf-8", "Liian monta pyyntöä – yritä hetken päästä uudelleen.");
  return false;
}

inline String esc(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;");  // pages also use '...' attributes (e.g. the login form's action)
  return s;
}

inline String head(const String& title) {
  String h;
  h.reserve(3000);
  h += F("<!doctype html><html lang=fi><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'><title>Kahvibotti</title><style>"
               "body{font-family:system-ui,sans-serif;max-width:40em;margin:0 auto;padding:0 1em 2em;background:#faf7f2;color:#2b2118}"
               "nav{display:flex;flex-wrap:wrap;gap:.4em 1em;padding:.8em 0;border-bottom:2px solid #6f4e37;margin-bottom:1em}"
               "nav a{color:#6f4e37;font-weight:600;text-decoration:none}"
               "h1{font-size:1.3em}.big{font-size:1.4em;font-weight:700;padding:.6em;background:#fff;border-radius:8px}"
               "table{border-collapse:collapse;width:100%}td{padding:.3em .2em;border-bottom:1px solid #e6ddd2;vertical-align:top}"
               "td:first-child{color:#7a6a5c;width:40%}input,button{font-size:1em;padding:.4em;width:100%;box-sizing:border-box}"
               "button{background:#6f4e37;color:#fff;border:0;border-radius:6px;margin-top:.5em}.note{color:#7a6a5c;font-size:.9em}"
               ".cal{table-layout:fixed;text-align:center}.cal td,.cal th{border:2px solid #faf7f2;padding:.25em 0;border-radius:6px}"
               ".cal td:first-child{width:auto;color:#7a6a5c;font-size:.8em}.cal .wt{font-weight:700;color:#6f4e37}.cal .d{display:block;font-size:.7em;color:#7a6a5c}"
               ".cal .hi{color:#fff}.cal .hi .d{color:#f3e6d6}.nav2{display:flex;justify-content:space-between;align-items:center}"
               ".nav2 a{color:#6f4e37;text-decoration:none;font-size:1.3em;padding:0 .5em}"
               ".heat{overflow-x:auto}.heat table{border-collapse:separate;border-spacing:2px;width:auto}"
               ".heat td{width:10px;height:10px;padding:0;border:0;border-radius:2px}.bar{background:#c89b6d;height:.9em;border-radius:3px}"
               ".warn{background:#fdecea;border-left:4px solid #9b2c2c;color:#7a1f1f;padding:.7em;border-radius:6px;margin:1em 0}"
               "details{margin:.4em 0;background:#fff;border-radius:6px;padding:.4em .7em}"
               "summary{font-weight:600;cursor:pointer;color:#6f4e37}details ul{padding-left:1.2em}"
               "</style><nav><a href=/>Info</a><a href=/tilastot>Tilastot</a>");
  h += loggedIn() ? F("<a href=/huolto>Huolto</a><a href=/wifi>Wi-Fi</a><a href=/ohjelmisto>Ohjelmisto</a>"
                      "<a href=/ulos style='margin-left:auto'>Kirjaudu ulos</a>")
                  : F("<a href=/kirjaudu style='margin-left:auto'>Ylläpito</a>");
  h += F("</nav><h1>");
  h += title;
  h += "</h1>";
  return h;
}

// Every streamed page goes out through part(). A client that stops reading (a phone locking its screen
// mid-page) makes each network write wait up to 10 s (NetworkClient: 10 retries x 1 s) and a chunk is 3
// writes; the main loop would sit there until the 60 s watchdog restarted the board. So: reset the watchdog
// before each piece, and drop the client when one piece takes over STALL_MS (it isn't reading: a moving
// download sends a 2 KB piece in milliseconds), or the whole response over PAGE_MAX_MS. A slow but moving
// download (a full-history CSV is ~2 MB) still finishes.
constexpr int64_t STALL_MS = 10000, PAGE_MAX_MS = 180000;
int64_t pageStart = 0;
bool pageDead = false;
inline void pageBegin(const char* type) {  // headers for a streamed response; starts the page's clock
  pageStart = nowMs();
  pageDead = false;
  server.sendHeader("Cache-Control", "no-store");  // always show the live state, never a cached page
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, type, "");
}
inline void part(const char* p, size_t n) {
  if (pageDead) return;
  esp_task_wdt_reset();
  int64_t t = nowMs();
  if (t - pageStart <= PAGE_MAX_MS) server.sendContent(p, n);
  if (nowMs() - t > STALL_MS || t - pageStart > PAGE_MAX_MS) {
    pageDead = true;
    server.client().stop();
    Serial.println("!!! web: client stopped reading, page dropped");
  }
}
inline void part(const String& s) { part(s.c_str(), s.length()); }
inline void pageEnd() { if (!pageDead) server.sendContent(""); }  // the terminating chunk

inline void send(const String& title, const String& body) {  // head and body sent separately: no joined copy
  pageBegin("text/html");
  part(head(title));
  part(body);
  pageEnd();
}

// For big pages (Tilastot's year view is ~36 KB): send in ~2 KB pieces as the page is built, instead of
// holding it all in one String. That big block, plus a Telegram TLS handshake (~40 KB) at the same time,
// took free RAM down to 22 KB. Use like a String: `page += "..."`, then end().
struct Chunked {
  String buf;
  explicit Chunked(const String& title) {
    pageBegin("text/html");
    buf.reserve(2400);
    buf = head(title);
  }
  Chunked& operator+=(const String& s) {
    buf += s;
    if (buf.length() > 2000) { part(buf); buf = ""; }
    return *this;
  }
  Chunked& operator+=(const char* s) { return *this += String(s); }
  void end() {
    if (buf.length()) part(buf);
    pageEnd();
  }
};

// After a POST, send the browser to a plain GET of the page ("post/redirect/get"), so a reload never
// repeats an action (restart, upload, delete) and forms never post to a stale ?a=... address.
inline void backTo(const char* page) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", page);
  server.send(303);
}

inline String num(float c);  // below, with the Tilastot helpers
// "pannullinen" = a full 10-cup pot's worth of coffee (cups / 10).
inline String pans(float cups) { float p = cups / 10; return fabsf(p - 1) < .05f ? String("1 pannullinen") : num(p) + " pannullista"; }

// "25.9.2026 klo 13.29" / "25.9. 13.29.05". Built by hand: the ESP32's strftime has no "%-d"
// (a glibc extension) and printed garbage.
inline String fiDate(const struct tm& t, bool year, bool secs) {
  char b[40];
  if (year) snprintf(b, sizeof b, "%d.%d.%d klo %d.%02d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min);
  else if (secs) snprintf(b, sizeof b, "%d.%d. %d.%02d.%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min, t.tm_sec);
  else snprintf(b, sizeof b, "%d.%d. %d.%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
  return b;
}

inline String row(const String& k, const String& v) { return "<tr><td>" + k + "</td><td>" + v + "</td></tr>"; }

inline String uptime() {
  int64_t s = esp_timer_get_time() / 1000000;
  char b[40];
  snprintf(b, sizeof b, "%lld pv %lld h %lld min", s / 86400, s / 3600 % 24, s / 60 % 60);
  return b;
}

// The public part of the Info page under the coffee status: what this is, what the answers mean, how to
// keep it accurate, the handling warning. Native <details> fold-outs keep it short (no scripts).
inline String infoText() {
  return F(
    "<div class=warn><b>⚠ Ole varovainen kahvibotti alustan kanssa.</b> Painosensorit ovat herkkiä eivätkä kestä "
    "iskuja. Laske kahvinkeitin aina rauhallisesti alustalle. Älä laita alustalle yli 10 kg painoa, sillä "
    "sensorit voivat rikkoutua.</div>"
    "<h2>Mikä kahvibotti on?</h2>"
    "<p>Kahvibotti punnitsee kahvinkeitintä sen alla olevalla alustalla ja päättelee painon muutoksien perusteella "
    "keitetäänkö kahvia ja paljonko sitä on lasikannussa. Kirjoita Telegram-ryhmään <b>/kahvi</b>, niin botti "
    "vastaa muutamassa sekunnissa. Tilastot-sivulta näet paljonko kahvia on keitetty päivittäin, viikoittain ja "
    "kuukausittain.</p>"
    "<details><summary>Mitä vastaukset tarkoittavat?</summary><ul>"
    "<li><b>Kahvi keittymässä – n. 6 kuppia, valmis ~3 min</b>: keitin on päällä, ja botti arvioi, milloin kahvi on "
    "valmista.</li>"
    "<li><b>6 kuppia (keitetty klo 9.14)</b>: lasikannussa on kahvia noin tämän verran, kahden kupin tarkkuudella "
    "(2, 4, 6, 8 tai 10).</li>"
    "<li><b>Tyhjä</b>: lasikannussa on alle kupillinen.</li>"
    "<li><b>Pannu on jonkun kädessä</b>: lasikannu on nostettu pois, joku kaataa kahvia.</li>"
    "<li><b>Pannu ei ole paikallaan</b>: lasikannu on ollut poissa yli 15 minuuttia (esimerkiksi tiskissä).</li>"
    "<li><b>Kahvinkeitin ei ole paikallaan</b>: koko keitin on nostettu pois alustalta.</li>"
    "<li><b>Vaaka ei vastaa</b>: painosensoreissa on jotain vikaa. Ilmoita ylläpitäjälle asiasta.</li></ul></details>"
    "<details><summary>Näin mittaus pysyy tarkkana</summary><ul>"
    "<li>Laita lasikannu aina keskelle lämpölevyä.</li>"
    "<li>Pidä kannet paikallaan: vesisäiliön kansi, suppilon kansi ja lasikannun kansi.</li>"
    "<li>Älä jätä mukeja tai muita tavaroita alustalle tai keittimen päälle.</li>"
    "<li>Älä nojaa keittimeen tai alustaan.</li>"
    "<li>Jos siirrät keitintä, laske se täsmällisesti takaisin paikalleen: keittimen neljä jalkaa alustan koloihin "
    "ja keittimen reunat tasan alustan reunojen kanssa.</li>"
    "<li><b>Keitin ja alusta oikein päin:</b> alustassa on kirjaimet L (vasen) ja R (oikea). Kun katsot keitintä "
    "edestä, L:n pitää olla vasemmalla ja R:n oikealla puolella.</li>"
    "<li>Alusta on tasaisella ja puhtaalla pinnalla.</li>"
    "<li>Kupit lasketaan keittimen vesisäiliön asteikolla: 1 kuppi = 1,25 dl, täysi pannu = 10 kuppia.</li>"
    "<li>Arvio olettaa, että kuppia kohden käytetään noin 7g kahvipuruja, 1 pieni kahvimitta. Jos puruja käytetään "
    "liikaa mittatarkkuus kärsii.</li>"
    "</ul></details>"
    "<details><summary>Jos jokin näyttää väärältä</summary><p>Tarkista ensin, että kaikki kohdassa "
    "\"Näin mittaus pysyy tarkkana\" mainitut asiat ovat kunnossa. Nosta sitten lasikannu hetkeksi pois ja laita se takaisin: "
    "botti mittaa kannun sisällön uudelleen. Jos vika jatkuu, ilmoita ylläpitäjälle.</p></details>");
}

inline void pageTila() {
  if (!publicOk()) return;
  bool adm = loggedIn();
  char reply[160];
  replyText(reply, sizeof reply);
  xSemaphoreTake(coreLock, portMAX_DELAY);
  bool machine = !core.machineAway, carafe = core.carafeOn;
  float pot = core.potG;
  xSemaphoreGive(coreLock);

  String b = "<p class=big>" + esc(reply) + "</p><table>";
  char v[80];
  snprintf(v, sizeof v, "%.0f kuppia", statTotal);
  b += row("Keitetty yhteensä", String(v) + " (≈ " + pans(statTotal) + ")");
  const stats::Milestone* g = stats::nextGoal(statTotal);
  if (g) snprintf(v, sizeof v, "%d kuppia (%.0f jäljellä)", g->cups, g->cups - statTotal);
  else snprintf(v, sizeof v, "kaikki saavutettu");
  b += row("Seuraava tavoite", v);
  if (clockValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    b += row("Aika", fiDate(t, true, false));
  }
  b += "</table>";
  b += infoText();
  if (!adm) { send("Info", b); return; }  // everyone on the network sees the above; the rest is for admins
  b += "<h2>Tekninen tila (ylläpitäjille)</h2><table>";
  b += row("Kahvinkeitin", machine ? "paikallaan" : "poissa");
  b += row("Lasikannu", carafe ? "paikallaan" : "poissa");
  // No basket row: lifting only the grounds out (basket left in place) looks the same to the scale as
  // lifting the basket, so the core's basketOn can't tell whether the suodatinsuppilo itself is there.
  snprintf(v, sizeof v, "~%.0f g", pot);
  b += row("Lasikannussa kahvia (arvio)", v);
  snprintf(v, sizeof v, "L %.0f &nbsp; R %.0f &nbsp; yht. %.0f", lastL, lastR, lastL + lastR);
  b += row("Vaaka (raaka)", v);
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(v, sizeof v, "%s, %d dBm, %s", esc(WiFi.SSID()).c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
    b += row("Wi-Fi", v);
  } else {
    b += row("Wi-Fi", "ei yhteyttä");
  }
  if (wifisetup::portalOn) b += row("Setup-verkko", wifisetup::AP_NAME);
  if (!clockValid()) b += row("Aika", "ei vielä tiedossa");
  b += row("Käynnissä", uptime());
  const esp_partition_t* run = esp_ota_get_running_partition();
  String fw = run && String(run->label) == "app1" ? "päivitetty versio (app1)" : "alkuperäinen (app0)";
  b += row("Ohjelmisto", fw + ", käännetty " + __DATE__ + " " + __TIME__);
  // Working memory (RAM), in plain words: a verdict first, the numbers below.
  unsigned freeK = ESP.getFreeHeap() / 1024, totalK = ESP.getHeapSize() / 1024, minK = ESP.getMinFreeHeap() / 1024,
           blockK = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024;
  bool memOk = freeK >= 50 && blockK >= 30;
  snprintf(v, sizeof v, "%s", memOk ? "<b>✓ kunnossa</b>" : "<b style='color:#9b2c2c'>⚠ vähissä</b>");
  char d[512];  // the text below is ~330 bytes (ä/ö are 2 bytes each)
  snprintf(d, sizeof d, "<br><span class=note>Vapaana %u kt / %u kt. Vähimmillään %u kt vapaana käynnistyksen jälkeen. "
           "Suurin yhtenäinen vapaa alue %u kt (verkkoyhteydet tarvitsevat noin 16 kt yhtenäistä tilaa). Jos muisti käy "
           "vähiin, laite käynnistyy itse uudelleen. Lisäksi laite käynnistyy uudelleen joka sunnuntai klo 4.</span>",
           freeK, totalK, minK, blockK);
  b += row("Käyttömuisti (RAM)", String(v) + d);
  // Each program part's own stack: the most it has ever used out of what it has.
  auto used = [](TaskHandle_t h, uint32_t size) { return (size - uxTaskGetStackHighWaterMark(h)) / 1024.0f; };
  float us = used(sensorTaskHandle, SENSOR_STACK), ut = used(telegramTaskHandle, TELEGRAM_STACK),
        uw = used(nullptr, getArduinoLoopTaskStackSize());
  bool stackOk = SENSOR_STACK / 1024.0f - us >= 1 && TELEGRAM_STACK / 1024.0f - ut >= 1 &&
                 getArduinoLoopTaskStackSize() / 1024.0f - uw >= 1;
  auto kt = [](float k) { String t = String(k, 1); t.replace(".", ","); return t; };  // "2,1" (Finnish decimal)
  String sd = String(stackOk ? "<b>✓ kunnossa</b>" : "<b style='color:#9b2c2c'>⚠ jokin lähellä rajaansa</b>") +
              "<br><span class=note>Kullakin ohjelman osalla on oma varattu muistialueensa. Suurin tähän asti "
              "käytetty määrä / varattu: vaa'an luku " + kt(us) + " / " + String(SENSOR_STACK / 1024) + " kt, Telegram " +
              kt(ut) + " / " + String(TELEGRAM_STACK / 1024) + " kt, verkkosivut " + kt(uw) + " / " +
              String(getArduinoLoopTaskStackSize() / 1024) + " kt. Kaikki on kunnossa, kun jokaisella on vähintään 1 kt varaa.</span>";
  b += row("Ohjelman osien muistinkäyttö", sd);
  if (!fsOk) b += row("Tilastomuisti", "<b style='color:#9b2c2c'>ei käytettävissä</b> (ei alustettu uudelleen, jotta tiedot eivät katoa)");
  b += "</table>";
  send("Info", b);
}

// ---------- Tilastot ----------

// Streams the stats file into fn, one record at a time (the history never has to fit in RAM).
// Reads 64 records (512 B) per call: 10 years of brews (~40 000) is ~600 reads instead of 40 000. Resets the
// watchdog as it goes, since the caller (a web page) may stream a slow client meanwhile.
// Records [from, to) of the file (default: all).
inline stats::Brew brewBuf[64];  // one shared buffer, off the stack; only the main loop (web + stats) reads
template <typename F> inline void eachBrew(F fn, const char* path = STATS_FILE, size_t from = 0, size_t to = SIZE_MAX) {
  File f = LittleFS.open(path, "r");
  if (f && from && !f.seek(from * sizeof(stats::Brew))) from = to;  // past the end: nothing to read
  stats::Brew* b = brewBuf;
  for (size_t i = from; f && i < to;) {
    size_t want = to - i < 64 ? to - i : 64;
    size_t n = f.read((uint8_t*)b, want * sizeof b[0]) / sizeof b[0];
    if (!n) break;
    for (size_t k = 0; k < n; k++) fn(b[k]);
    i += n;
    esp_task_wdt_reset();
  }
  if (f) f.close();
}

// One Tilastot view of a stats file (stats::fillPage over the file). The result lives in one static Page
// (~1.6 KB off the stack), valid until the next call. Main loop only.
inline const stats::Page& statsPage(const char* path, bool ordered, const stats::Today& n, int y, int m, bool yearView) {
  static stats::Page p;
  File f = LittleFS.open(path, "r");
  if (f) f.setBufferSize(sizeof(stats::Brew));  // for the binary search's 8-byte reads, not a 4 KB refill each
  size_t count = f ? f.size() / sizeof(stats::Brew) : 0;
  auto at = [&](size_t i) -> uint32_t {  // epoch of record i
    stats::Brew b = {};
    if (f.seek(i * sizeof b)) f.read((uint8_t*)&b, sizeof b);
    return b.epoch;
  };
  auto each = [&](size_t from, size_t to, auto fn) { eachBrew(fn, path, from, to); };
  stats::fillPage(count, ordered, at, each, n, y, m, yearView, p);
  if (f) f.close();
  return p;
}

inline const char* heat(float c, float max) {  // 5-step coffee scale
  if (c <= 0) return "#f1ebe3";
  float r = c / (max > 1 ? max : 1);
  return r < .25f ? "#e8d3b8" : r < .5f ? "#d2a877" : r < .75f ? "#a8743f" : "#6f4e37";
}

inline String num(float c) {  // "14" or "2,5" (Finnish decimal comma)
  char b[16];
  if (fabsf(c - lroundf(c)) < .05f) snprintf(b, sizeof b, "%ld", lroundf(c));
  else snprintf(b, sizeof b, "%.1f", c);
  String s = b;
  s.replace(".", ",");
  return s;
}

inline void pageStats() {
  if (!publicOk()) return;
  if (!clockValid()) { send("Tilastot", "<p>Kellonaika ei ole vielä tiedossa – odota hetki.</p>"); return; }
  time_t now = time(nullptr);
  struct tm tn;
  localtime_r(&now, &tn);
  int ty = tn.tm_year + 1900, tm_ = tn.tm_mon + 1, td = tn.tm_mday;
  int y = server.hasArg("y") ? server.arg("y").toInt() : ty;
  int m = server.hasArg("m") ? server.arg("m").toInt() : tm_;
  if (m < 1 || m > 12 || y < 2000 || y > 2200) { y = ty; m = tm_; }
  bool yearView = server.arg("v") == "vuosi";

  // Only the part of the history this view shows is read: constant time, however many years are stored.
  // The all-time total is the running sum kept since boot.
  const stats::Page& p = statsPage(STATS_FILE, statsOrdered, stats::Today(ty, tm_, td), y, m, yearView);
  const stats::Totals& t = p.t;
  const float *mc = p.month, *yd = p.yearDay, *ym = p.yearMonth;
  float all = statTotal;

  Chunked h("Tilastot");  // streamed: see Chunked
  h += "<table>";
  h += row("Tänään", num(t.today) + " kuppia");
  h += row("Viikko " + String(stats::isoWeek(ty, tm_, td)), num(t.week) + " kuppia");
  h += row(String(stats::monthName(tm_)), num(t.month) + " kuppia");
  h += row("Vuosi " + String(ty), num(t.year) + " kuppia");
  h += row("Lukuvuosi " + String(stats::acadYear(ty, tm_)) + "–" + String(stats::acadYear(ty, tm_) + 1),
           num(t.acad) + " kuppia");
  h += row("Kaikkiaan", num(all) + " kuppia (≈ " + pans(all) + ")");
  h += "</table>";

  if (!yearView) {
    int py = m == 1 ? y - 1 : y, pm = m == 1 ? 12 : m - 1, ny = m == 12 ? y + 1 : y, nm = m == 12 ? 1 : m + 1;
    h += "<h2 class=nav2><a href='/tilastot?y=" + String(py) + "&m=" + String(pm) + "'>‹</a>" +
         String(stats::monthName(m)) + " " + String(y) +
         "<a href='/tilastot?y=" + String(ny) + "&m=" + String(nm) + "'>›</a></h2>";
    float mx = 0, sum = 0;
    for (int d = 1; d <= 31; d++) { mx = fmaxf(mx, mc[d]); sum += mc[d]; }
    int rows[6][8];
    int n = stats::monthGrid(y, m, rows);
    h += "<table class=cal><tr><th>vko</th><th>ma</th><th>ti</th><th>ke</th><th>to</th><th>pe</th><th>la</th><th>su</th><th>yht.</th></tr>";
    for (int r = 0; r < n; r++) {
      float wk = 0;
      for (int c = 1; c <= 7; c++) wk += rows[r][c] ? mc[rows[r][c]] : 0;
      h += "<tr><td>" + String(rows[r][0]) + "</td>";
      for (int c = 1; c <= 7; c++) {
        int d = rows[r][c];
        if (!d) { h += "<td></td>"; continue; }
        float cu = mc[d];
        bool dark = cu > 0 && cu / (mx > 1 ? mx : 1) >= .5f;
        h += "<td" + String(dark ? " class=hi" : "") + " style='background:" + heat(cu, mx) + "' title='" +
             String(d) + "." + String(m) + ".: " + num(cu) + " kuppia'><span class=d>" +
             String(d) + "</span>" + (cu > 0 ? "<b>" + num(cu) + "</b>" : "&nbsp;") + "</td>";
      }
      h += "<td class=wt>" + (wk > 0 ? num(wk) : String("")) + "</td></tr>";
    }
    h += "</table><p>Kuukausi yhteensä <b>" + num(sum) + " kuppia</b> (≈ " + pans(sum) + "). "
         "Vasemmalla viikkonumero, oikealla (yht.) montako kuppia viikon aikana on juotu.</p>";
    h += "<p><a href='/tilastot?v=vuosi&y=" + String(y) + "'>Vuosinäkymä " + String(y) + " »</a></p>";
  } else {
    h += "<h2 class=nav2><a href='/tilastot?v=vuosi&y=" + String(y - 1) + "'>‹</a>" + String(y) +
         "<a href='/tilastot?v=vuosi&y=" + String(y + 1) + "'>›</a></h2>";
    float dmax = 0, mmax = 0, sum = 0;
    for (int i = 1; i <= 366; i++) dmax = fmaxf(dmax, yd[i]);
    for (int i = 1; i <= 12; i++) { mmax = fmaxf(mmax, ym[i]); sum += ym[i]; }
    static int16_t cell[54][7];  // day of year, 0 = outside the year
    for (int c = 0; c < 54; c++) for (int r = 0; r < 7; r++) cell[c][r] = 0;
    int cols = 0;
    for (int mm = 1; mm <= 12; mm++)
      for (int d = 1; d <= stats::daysInMonth(y, mm); d++) {
        int c, r;
        stats::yearCell(y, mm, d, c, r);
        cell[c][r] = stats::dayOfYear(y, mm, d);
        cols = c + 1;
      }
    h += "<div class=heat><table>";
    static const char* wd[] = {"ma", "", "ke", "", "pe", "", "su"};
    for (int r = 0; r < 7; r++) {
      h += "<tr><td style='width:auto;font-size:.7em;color:#7a6a5c'>" + String(wd[r]) + "</td>";
      for (int c = 0; c < cols; c++) {
        int doy = cell[c][r];
        if (!doy) { h += "<td></td>"; continue; }
        int mo = 1, da = doy;
        while (da > stats::daysInMonth(y, mo)) da -= stats::daysInMonth(y, mo++);
        h += "<td style='background:" + String(heat(yd[doy], dmax)) + "' title='" + String(da) + "." + String(mo) +
             ".: " + num(yd[doy]) + " kuppia'></td>";
      }
      h += "</tr>";
    }
    h += "</table></div><table>";
    for (int mm = 1; mm <= 12; mm++)
      h += "<tr><td><a href='/tilastot?y=" + String(y) + "&m=" + String(mm) + "'>" + String(stats::monthName(mm)) +
           "</a></td><td><div class=bar style='width:" + String(mmax > 0 ? (int)(ym[mm] * 100 / mmax) : 0) +
           "%'></div></td><td style='width:4em;text-align:right'>" + num(ym[mm]) + "</td></tr>";
    h += "</table><p>Vuosi yhteensä <b>" + num(sum) + " kuppia</b>.</p>";
    h += "<p><a href='/tilastot?y=" + String(y) + "&m=" + String(y == ty ? tm_ : 1) + "'>« Kuukausinäkymä</a></p>";
  }
  h += "<p class=note>1 kuppi = 1,25 dl vettä keittimen vesisäiliön asteikolla.<br>"
       "1 pannu = täysi 10 kupin pannu eli 1,25 l vettä (vesisäiliö täytettynä 10 kupin viivaan asti).<br>"
       "Luvut kertovat arvion siitä paljonko kahvia on päätynyt pannuun. Pannuun tulee valmista kahvia hieman "
       "vähemmän kuin vesiastiaan on laitettu vettä, koska osa vedestä imeytyy kahvipuruihin ja suodatinpussiin ja "
       "osa haihtuu keittäessä.<br>"
       "<a href=/tilastot.csv>Lataa kaikki keitot (CSV)</a></p>";
  h.end();
}

inline void pageCsv() {
  if (!publicOk()) return;
  server.sendHeader("Content-Disposition", "attachment; filename=kahvibotti_keitot.csv");
  pageBegin("text/csv; charset=utf-8");
  part("pvm;klo;vesi_g;kupit\r\n");
  // ~1.5 KB per network write: one write per line made 10 years of brews a ~40 000-packet download.
  static char out[1600];
  size_t used = 0;
  eachBrew([&](const stats::Brew& b) {
    if (pageDead) return;
    time_t e = b.epoch;
    struct tm t;
    localtime_r(&e, &t);
    used += snprintf(out + used, sizeof out - used, "%d.%d.%d;%d.%02d;%u;%s\r\n", t.tm_mday, t.tm_mon + 1,
                     t.tm_year + 1900, t.tm_hour, t.tm_min, b.waterG, num(stats::cups(b)).c_str());
    if (used > sizeof out - 64) { part(out, used); used = 0; }
  });
  if (used) part(out, used);
  pageEnd();
}

// Serial "bench N": how Tilastot, the CSV and the boot-time sum cope with N brews (40 000 = ~11 a day for 10
// years). Uses a scratch file (/bench.bin, deleted after), never the real stats. Main loop only.
inline void statsBench(int n) {
  if (n < 1 || n > 120000) n = 40000;
  size_t freeFs = LittleFS.totalBytes() - LittleFS.usedBytes();
  if ((size_t)n * sizeof(stats::Brew) + 16384 > freeFs) { Serial.printf(">>> bench: only %u bytes free on flash\n", freeFs); return; }
  size_t heap0 = ESP.getFreeHeap();
  int64_t t0 = nowMs();
  File f = LittleFS.open("/bench.bin", "w");
  uint32_t now = time(nullptr) > 1700000000 ? time(nullptr) : 1790000000u;
  static stats::Brew b[64];
  for (int i = 0; i < n;) {
    int k = 0;
    for (; k < 64 && i < n; k++, i++) b[k] = {now - (uint32_t)((int64_t)(n - i) * 315360000 / n), (uint16_t)(250 + 125 * (i % 9)), 0};
    f.write((uint8_t*)b, k * sizeof b[0]);
    esp_task_wdt_reset();
  }
  f.close();
  int64_t t1 = nowMs();
  float sum = 0;
  eachBrew([&](const stats::Brew& r) { sum += stats::cups(r); }, "/bench.bin");  // = boot-time total
  int64_t t2 = nowMs();
  time_t tn = now;
  struct tm lt;
  localtime_r(&tn, &lt);
  int ly = lt.tm_year + 1900, lm = lt.tm_mon + 1, ld = lt.tm_mday;
  stats::Totals t;
  static float mc[32], yd[367], ym[13];
  static int mb[32];
  eachBrew([&](const stats::Brew& r) {  // the old Tilastot: every record, three date conversions each
    stats::addTotals(r, ly, lm, ld, t);
    stats::addMonth(r, ly, lm, mc, mb);
    stats::addYear(r, ly, yd, ym);
  }, "/bench.bin");
  int64_t t3a = nowMs();
  float fast = 0;  // today's Tilastot (statsPage): this month's view and this year's
  for (bool yv : {false, true}) fast += statsPage("/bench.bin", true, stats::Today(ly, lm, ld), ly, lm, yv).t.year;
  int64_t t3 = nowMs();
  size_t csv = 0;
  eachBrew([&](const stats::Brew& r) {  // = the CSV download, minus the network
    time_t e = r.epoch;
    struct tm c;
    localtime_r(&e, &c);
    char line[64];
    csv += snprintf(line, sizeof line, "%d.%d.%d;%d.%02d;%u;%s\r\n", c.tm_mday, c.tm_mon + 1, c.tm_year + 1900,
                    c.tm_hour, c.tm_min, r.waterG, num(stats::cups(r)).c_str());
  }, "/bench.bin");
  int64_t t4 = nowMs();
  LittleFS.remove("/bench.bin");
  Serial.printf(">>> bench %d brews (%u KB): write %lld ms, boot sum %lld ms (%.0f cups), Tilastot before %lld ms (all %.0f), "
                "Tilastot now %lld ms per view (year %.0f = %.0f), CSV %lld ms (%u KB), heap %u -> %u, flash free %u KB of %u KB\n",
                n, n * 8 / 1024, t1 - t0, t2 - t1, sum, t3a - t2, t.all, (t3 - t3a) / 2, fast / 2, t.year, t4 - t3,
                csv / 1024, heap0, ESP.getFreeHeap(),
                freeFs / 1024, LittleFS.totalBytes() / 1024);
}

// ---------- Huolto ----------

String huoltoMsg;
int64_t restartAt = 0;
uint8_t restartWhy = 0;  // fw::PLANNED_*; 0 = from the panel

String fwMsg;

// A complete, checked firmware upload (app1 is set to boot): note it and restart into it.
inline void uploadAccepted() {
  fw::clearFailureFlag();  // a new upload replaces any earlier failure notice (and keeps the upload!)
  fwMsg = "Päivitys ladattu ja tarkistettu. Käynnistetään uuteen versioon – päivitä sivu noin 30 s kuluttua "
          "(jos kahvia keitetään juuri, vasta keiton jälkeen).";
  xSemaphoreTake(coreLock, portMAX_DELAY);
  logEvent(nowMs(), "Ohjelmistopäivitys ladattu", 0, 0);
  xSemaphoreGive(coreLock);
  restartAt = nowMs() + 3000;
}

inline String wallTime(int64_t ms) {  // an event's time as "25.9. 13.29.05" (or uptime if no clock)
  if (!clockValid()) { char b[32]; snprintf(b, sizeof b, "+%lld s", ms / 1000); return b; }
  time_t e = time(nullptr) - (nowMs() - ms) / 1000;
  struct tm t;
  localtime_r(&e, &t);
  return fiDate(t, false, true);
}

inline void pageHuolto() {
  if (!admin()) return;
  if (server.method() == HTTP_POST) {
    String a = server.arg("a");
    if (a == "reset") {
      xSemaphoreTake(coreLock, portMAX_DELAY);
      core.freshStart();
      logEvent(nowMs(), "Tila nollattu käsin (Huolto)", 0, 0);
      xSemaphoreGive(coreLock);
      huoltoMsg = "Kahvitila nollattu. Botti olettaa nyt keittimen olevan ohjeen mukaisessa tilassa: "
                  "tyhjä lasikannu ja tyhjä suodatinsuppilo paikallaan, vesisäiliö tyhjä.";
    } else if (a == "restart") {
      huoltoMsg = "Käynnistetään uudelleen… Päivitä sivu noin 20 sekunnin päästä (jos kahvia keitetään juuri, vasta "
                  "keiton jälkeen).";
      restartAt = nowMs() + 3000;  // let the redirect and the page reach the browser first
    } else if (a == "tgtest") {
      xSemaphoreTake(coreLock, portMAX_DELAY);
      tgTestPending = true;
      tgTestResult = "";
      xSemaphoreGive(coreLock);
      huoltoMsg = "Testiviesti lähetetään ryhmään… Päivitä sivu muutaman sekunnin päästä nähdäksesi tuloksen.";
    } else if (a == "tggroup") {
      String g = server.arg("g");
      g.trim();
      bool valid = false;
      if (g.length() >= 5 && g.length() <= 16) {
        valid = true;
        for (unsigned i = 0; i < g.length(); i++)
          if (!(isDigit(g[i]) || (i == 0 && g[i] == '-'))) valid = false;
      }
      if (!valid) {
        huoltoMsg = "Ryhmää ei vaihdettu: tunnus on numero, ryhmillä miinusmerkkinen (esim. -1001234567890).";
      } else {
        Preferences pr;
        pr.begin("tg", false);
        pr.putString("group", g);
        pr.end();
        xSemaphoreTake(coreLock, portMAX_DELAY);
        loadTgGroup();
        String now = tgGroup;
        logEvent(nowMs(), "Telegram-ryhmä vaihdettu", 0, 0);
        milestoneNextTry = 0;  // a milestone waiting out a "wrong group" pause is tried at once
        xSemaphoreGive(coreLock);
        huoltoMsg = "Virstanpylväsviestit menevät nyt ryhmään " + now + ". Kokeile Lähetä testiviesti -painiketta.";
      }
    } else if (a == "account") {
      String u = server.arg("u"), p1 = server.arg("p1"), p2 = server.arg("p2");
      u.trim();
      if (u.length() < 1 || u.length() > 32 || u.indexOf(':') >= 0) {
        huoltoMsg = "Tunnusta ei vaihdettu: käyttäjätunnuksessa pitää olla 1–32 merkkiä, eikä siinä saa olla kaksoispistettä.";
      } else if (u == ADMIN_USER) {
        huoltoMsg = "Tunnusta ei vaihdettu: valitse eri käyttäjätunnus kuin laitteen omistajan varatunnus.";
      } else if (p1 != p2) {
        huoltoMsg = "Tunnusta ei vaihdettu: salasanat eivät täsmää.";
      } else if (p1.length() < 8 || p1.length() > 63) {
        huoltoMsg = "Tunnusta ei vaihdettu: salasanassa pitää olla 8–63 merkkiä (se on myös setup-verkon salasana).";
      } else {
        // Password first, user name last: a power cut in between leaves the old name with the new password
        // (the user knows both), never a new name with an old password nobody may remember.
        Preferences pr;
        pr.begin("admin", false);
        pr.putString("pass", p1);
        pr.putString("user", u);
        pr.end();
        loadPanelAccount();
        if (panelUser != u || panelPass != p1) {
          huoltoMsg = "Tunnuksen tallennus epäonnistui – kokeile uudelleen. Salasana (myös setup-verkon) saattoi jo "
                      "vaihtua.";
          return backTo("/huolto");
        }
        Session* me = currentSession();
        for (Session& x : sessions) if (&x != me) x.tok = "";  // everyone else logs in again
        xSemaphoreTake(coreLock, portMAX_DELAY);
        logEvent(nowMs(), "Ylläpitäjän tunnus vaihdettu", 0, 0);
        xSemaphoreGive(coreLock);
        huoltoMsg = "Ylläpitäjän tunnus tallennettu. Kirjaudu jatkossa uudella tunnuksella ja salasanalla. Sama "
                    "salasana on nyt myös EmuKahviBottiHotspot-verkon salasana.";
      }
    } else if (a == "clear") {
      if (server.arg("confirm") != "TYHJENNÄ") {
        huoltoMsg = "Tilastoja ei tyhjennetty: kirjoita TYHJENNÄ vahvistukseksi.";
      } else if (!fsOk || (LittleFS.exists(STATS_FILE) && !LittleFS.remove(STATS_FILE))) {
        huoltoMsg = "Tilastojen tyhjennys epäonnistui (tiedostoa ei voitu poistaa). Mitään ei muutettu.";
      } else {
        xSemaphoreTake(coreLock, portMAX_DELAY);  // milestones() reads these from the Telegram task
        statTotal = 0;
        statBrews = 0;
        statsOrdered = true;  // an empty file is in order
        statLastEpoch = 0;
        msAnnounced = 0;  // milestones count again from zero
        Preferences p;
        p.begin("stats", false);
        p.putInt("ms", 0);
        p.end();
        logEvent(nowMs(), "Tilastot tyhjennetty (Huolto)", 0, 0);
        xSemaphoreGive(coreLock);
        huoltoMsg = "Tilastot tyhjennetty.";
      }
    }
    backTo("/huolto");
    return;
  }
  String h;
  if (huoltoMsg.length()) { h += "<p class=big>" + esc(huoltoMsg) + "</p>"; huoltoMsg = ""; }
  h += "<p class=big id=live>…</p><p class=note>Vaa'an raakalukemat (L, R, yhteensä) päivittyvät 2 s välein. "
       "Muutokset merkitsevät, eivät nollakohta.</p>"
       "<script>setInterval(()=>fetch('/huolto/live').then(r=>r.text()).then(t=>live.textContent=t),2000)</script>";
  h += "<h2>Toiminnot</h2>"
       "<form method=post action=/huolto onsubmit=\"return confirm('Onko keitin ohjeen mukaisessa tilassa? Nollataanko kahvitila?')\">"
       "<input type=hidden name=a value=reset><p>Käytä, jos botti on jumissa väärässä tilassa (esim. väittää lasikannun "
       "olevan poissa). <b>Laita keitin ensin tähän tilaan:</b></p><ul>"
       "<li>alusta on tasaisella ja puhtaalla pinnalla</li>"
       "<li>keittimen neljä jalkaa alustan koloihin ja keittimen reunat tasan alustan reunojen kanssa</li>"
       "<li>suodatinsuppilo paikallaan tyhjänä, suppilon kansi päällä</li>"
       "<li>vesisäiliö tyhjä, vesisäiliön kansi päällä</li>"
       "<li>lasikannu paikallaan tyhjänä, lasikannun kansi päällä</li></ul>"
       "<button>Nollaa kahvitila</button><p class=note>Nollauksen jälkeen botti olettaa keittimen olevan juuri tässä "
       "tilassa ja laskee kaiken siitä eteenpäin.</p></form>"
       "<form method=post action=/huolto onsubmit=\"return confirm('Käynnistetäänkö uudelleen?')\"><input type=hidden name=a value=restart>"
       "<button>Käynnistä uudelleen</button><p class=note>Tila, tilastot ja Wi-Fi-asetukset säilyvät. "
       "Yhteys tähän sivuun katkeaa uudelleenkäynnistyksen ajaksi (noin 20–30 s) – päivitä sivu sen jälkeen. "
       "Telegram-botti ei vastaa sillä välin. Jos kahvia keitetään juuri, uudelleenkäynnistys odottaa keiton "
       "loppuun.</p></form>"
       "<form method=post action=/huolto><input type=hidden name=a value=clear>"
       "<p>Tyhjennä tilastot – kirjoita <b>TYHJENNÄ</b>:<br><input name=confirm autocomplete=off></p>"
       "<button style='background:#9b2c2c'>Tyhjennä tilastot</button><p class=note>Poistaa kaikki keitot ja "
       "aloittaa virstanpylväät alusta. Ei voi perua – lataa ensin CSV Tilastot-sivulta.</p></form>";
  size_t fsTotal = fsOk ? LittleFS.totalBytes() : 0;
  if (fsTotal) {  // ~100 000 brews fit (~25 years at 11 a day); when it's full, new brews are no longer saved
    int pct = (int)((uint64_t)LittleFS.usedBytes() * 100 / fsTotal);
    h += pct >= 90 ? "<p><b style='color:#9b2c2c'>⚠ Tilastotila on " + String(pct) + " % täynnä. Kun se täyttyy, uusia "
                     "keittoja ei enää tallenneta: lataa CSV talteen ja tyhjennä tilastot.</b></p>"
                   : "<p class=note>Tilastotilaa käytetty " + String(pct) + " %.</p>";
  }
  {  // Telegram
    xSemaphoreTake(coreLock, portMAX_DELAY);
    int64_t lastOk = tgLastOk;
    String botUser = tgBotUser, group = tgGroup, testRes = tgTestResult, pollErr = tgPollErr;
    int today = tgAnswersToday;
    bool testPending = tgTestPending;
    xSemaphoreGive(coreLock);
    auto ago = [](int64_t ms) {
      int64_t m = (nowMs() - ms) / 60000;
      return m < 1 ? String("juuri nyt") : m < 120 ? String((long)m) + " min sitten" : String((long)(m / 60)) + " h sitten";
    };
    bool ok = lastOk && nowMs() - lastOk < 10 * 60000;
    h += "<h2>Telegram</h2><table>";
    h += row("Yhteys", ok ? "<b>✓ toimii</b> (viimeksi " + ago(lastOk) + ")"
                          : String("<b style='color:#9b2c2c'>⚠ ei yhteyttä</b>") + (lastOk ? " (viimeksi " + ago(lastOk) + ")" : ""));
    if (pollErr.length()) h += row("Viestien haku", "<b style='color:#9b2c2c'>⚠ Telegram vastasi: " + esc(pollErr) + "</b>");
    h += row("Botti", botUser.length() ? "@" + esc(botUser) : String("–"));
    String tok = BOT_TOKEN;
    int colon = tok.indexOf(':');
    h += row("Botin token", (colon > 0 ? esc(tok.substring(0, colon)) + ":••••••••" : String("••••••••")));
    h += row("/kahvi-kyselyt tänään", String(today));
    Preferences pr;
    pr.begin("tg", true);
    bool custom = pr.isKey("group");
    pr.end();
    h += row("Ryhmä virstanpylväsviesteille", esc(group) + (custom ? " (asetettu täällä)" : " (oletus, secrets.h)"));
    h += "</table><p class=note>Virstanpylväsviestit ja testiviestit lähetetään vain tähän ryhmään. /kahvi-kyselyihin "
         "botti vastaa kaikissa ryhmissä, joissa se on.</p>";
    if (testPending) h += "<p class=note>Testiviesti on lähdössä… päivitä sivu hetken päästä.</p>";
    else if (testRes.length()) h += "<p class=big>" + esc(testRes) + "</p>";
    h += "<form method=post action=/huolto><input type=hidden name=a value=tgtest>"
         "<button>Lähetä testiviesti ryhmään</button></form>";
    h += "<form method=post action=/huolto onsubmit=\"return confirm('Oletko varma?')\">"
         "<input type=hidden name=a value=tggroup>"
         "<p>Ryhmän tunnus:<br><input name=g placeholder='-1001234567890' required></p>"
         "<button>Tallenna ryhmä</button></form>";
    h += "<details><summary>Näin löydät ryhmän tunnuksen</summary><ol>"
         "<li>Avaa Telegram Web osoitteessa <b>web.telegram.org/a/</b> tietokoneen selaimessa ja kirjaudu sisään "
         "(juuri a-versio: se näyttää isompien ryhmien tunnuksen oikeassa, -100 alkavassa muodossa).</li>"
         "<li>Klikkaa ryhmää, jonka tunnuksen haluat.</li>"
         "<li>Katso selaimen osoiteriviä (URL) ylhäällä.</li>"
         "<li>Kopioi risuaidan (#) jälkeen tulevat numerot. Ne alkavat yleensä miinusmerkillä "
         "(esimerkiksi #-123456789).</li></ol></details>";
  }
  h += "<h2>Ylläpitäjän tunnus</h2><table>" +
       row("Ylläpitäjä", panelUser.length() ? esc(panelUser) : String("ei vielä asetettu")) +
       row("Kirjautunut", isOwner() ? "laitteen omistajan varatunnuksella" : "ylläpitäjän tunnuksella") +
       "</table><form method=post action=/huolto onsubmit=\"return confirm('Oletko varma, että haluat vaihtaa tunnukset "
       "ja onko uudet tunnukset tallessa?')\"><input type=hidden name=a value=account>"
       "<p>Uusi käyttäjätunnus:<br><input name=u autocomplete=username required maxlength=32></p>"
       "<p>Uusi salasana (vähintään 8 merkkiä):<br><input name=p1 type=password autocomplete=new-password required></p>"
       "<p>Salasana uudelleen:<br><input name=p2 type=password autocomplete=new-password required></p>"
       "<button>Tallenna tunnus</button><p class=note>Tällä tunnuksella kirjaudutaan hallintasivuille, ja salasana on "
       "myös EmuKahviBottiHotspot-verkon salasana (se avautuu, jos Wi-Fi on ollut poikki 5 minuuttia). Laitteella on "
       "lisäksi omistajan varatunnus, jolla pääsee sisään jos admin käyttäjän tunnus unohtuu.</p></form>";
  // The event log last: it is long, and the buttons above are what people come here for.
  h += "<h2>Viimeisimmät tapahtumat</h2><table>";
  xSemaphoreTake(coreLock, portMAX_DELAY);
  int n = evCount;
  static Ev snap[EV_MAX];
  for (int i = 0; i < n; i++) snap[i] = evRing[(evHead - 1 - i + EV_MAX) % EV_MAX];  // newest first
  xSemaphoreGive(coreLock);
  for (int i = 0; i < n; i++) {
    String v;
    if (snap[i].a != 0 || snap[i].b != 0) v = String((long)lroundf(snap[i].a)) + (snap[i].b != 0 ? " / " + String((long)lroundf(snap[i].b)) : "");
    h += "<tr><td>" + wallTime(snap[i].ms) + "</td><td>" + esc(snap[i].what) + (v.length() ? "<br><span class=note>" + v + "</span>" : "") + "</td></tr>";
  }
  h += "</table>";
  send("Huolto", h);
}

inline void pageLive() {
  if (!admin()) return;
  char reply[160], b[240];
  replyText(reply, sizeof reply);
  snprintf(b, sizeof b, "L %.0f   R %.0f   yht. %.0f   —   %s", lastL, lastR, lastL + lastR, reply);
  server.send(200, "text/plain; charset=utf-8", b);
}

// ---------- Ohjelmisto (firmware) ----------

String upErr;
bool upOk = false, upAuthed = false;

// Upload callback of /ohjelmisto/lataa, called per chunk while the .bin arrives.
// WebServer also calls an upload callback for *non-multipart* POST bodies (as "raw" data, whenever a
// route has one: canRaw()). server.upload() then dereferences a null upload -> crash. That's what made
// every button on this page crash the board (2026-09-25). Hence: buttons post to /ohjelmisto (no
// callback), and this callback touches the upload only for real file uploads.
inline void onUpload() {
  if (!server.header("Content-Type").startsWith("multipart/form-data")) return;
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upErr = ""; upOk = false;
    upAuthed = loggedIn();
    if (!upAuthed) { upErr = "Ei kirjautunut."; return; }
    if (!fw::canUpload()) { upErr = "Päivityksen voi ladata vain alkuperäisen version ollessa käynnissä."; return; }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) upErr = "Päivitystä ei voitu aloittaa: " + String(Update.errorString());
    Serial.printf(">>> firmware upload started: %s\n", u.filename.c_str());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();  // ~1.3 MB takes several seconds
    if (upErr.length()) return;
    if (Update.write(u.buf, u.currentSize) != u.currentSize) upErr = "Kirjoitus epäonnistui: " + String(Update.errorString());
  } else if (u.status == UPLOAD_FILE_END) {
    if (upErr.length()) { Update.abort(); return; }
    if (Update.end(true)) upOk = true;  // validates the image and sets app1 as the boot slot
    else upErr = "Tiedosto ei kelpaa: " + String(Update.errorString());
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    // WebServer also reports ABORTED when the browser disconnects right *after* a complete file: the new
    // version is then already checked and set to start, and uploadDone() won't run. Finish it here.
    if (upOk) { uploadAccepted(); upOk = false; return; }
    Update.abort();
    upErr = "Lähetys keskeytyi.";
  }
}

// /ohjelmisto/lataa, after onUpload() has received the whole file.
inline void uploadDone() {
  if (!admin()) return;
  if (upOk) {
    uploadAccepted();
  } else {
    fwMsg = upErr.length() ? upErr : "Päivitys epäonnistui.";
  }
  upOk = false;
  backTo("/ohjelmisto");
}

inline void pageFirmware() {
  if (!admin()) return;
  if (server.method() == HTTP_POST) {
    String a = server.arg("a");
    if (a == "orig" || a == "origdel") {
      if (fw::bootOriginal(a == "origdel")) {
        fwMsg = a == "origdel" ? "Palataan alkuperäiseen versioon ja poistetaan päivitys…" : "Palataan alkuperäiseen versioon…";
        restartAt = nowMs() + 3000;
      } else {
        fwMsg = "Vaihto alkuperäiseen versioon epäonnistui. Laite jatkaa nykyisellä versiolla.";
      }
    } else if (a == "upd") {
      if (fw::bootUpdate()) {
        fwMsg = "Käynnistetään päivitettyyn versioon…";
        restartAt = nowMs() + 3000;
      } else {
        fwMsg = fw::hasFirmware(1) ? "Vaihto päivitettyyn versioon epäonnistui. Laite jatkaa nykyisellä versiolla."
                                   : "Päivitettyä versiota ei ole.";
      }
    } else if (a == "del") {
      fwMsg = fw::deleteUpdateNow() ? "Päivitys poistettu. Jäljellä on vain alkuperäinen versio." : "Poisto ei onnistunut.";
    } else if (a == "ok") {
      fw::ackFailure();
    } else if (a == "crashok") {
      fw::clearCrash();
    }
    backTo("/ohjelmisto");
    return;
  }
  bool upd = fw::runningUpdate();
  String h;
  if (fwMsg.length()) { h += "<p class=big>" + esc(fwMsg) + "</p>"; fwMsg = ""; }
  if (fw::fellBack)
    h += "<p class=big style='color:#9b2c2c'>Päivitetty versio nollautui ennen kuin ehti todeta toimivansa, joten "
         "botti palasi alkuperäiseen versioon. Kuittaus poistaa epäonnistuneen päivityksen. Nollauksen syy: " +
         String(fw::resetText(fw::fellBackReason)) + ".</p>"
         "<form method=post action=/ohjelmisto><button name=a value=ok>Kuittaa</button></form>";
  h += "<table>";
  h += row("Käynnissä", upd ? "<b>päivitetty versio</b>" : "<b>alkuperäinen versio</b>");
  h += row("Alkuperäinen (USB)", "käännetty " + fw::built(0) + "<br><span class=note>ei voi poistaa selaimesta</span>");
  h += row("Päivitetty (selaimesta)", fw::hasFirmware(1) ? "käännetty " + fw::built(1) : String("ei ole"));
  h += "</table>";
  String crash = fw::crashInfo();
  if (crash.length())
    h += "<p class=note><b>Viimeisin kaatuminen:</b> " + esc(crash) +
         "</p><form method=post action=/ohjelmisto><button name=a value=crashok style='width:auto'>Tyhjennä kaatumistieto</button></form>";
  h += "<h2>Viimeisimmät käynnistykset</h2><table>";
  for (int i = 0; i < fw::histN; i++)
    h += row(i == 0 ? "nyt" : String(i) + ". edellinen", String(fw::hist[i] & 0x8000 ? "päivitetty" : "alkuperäinen") +
             " – edellinen ajo päättyi: " + fw::resetText(fw::hist[i] & 0xff) + fw::plannedText((fw::hist[i] >> 8) & 0x7f));
  h += "</table>";
  auto btn = [](const char* a, const char* label, const char* ask) {
    return String("<form method=post action=/ohjelmisto onsubmit=\"return confirm('") + ask + "')\"><button name=a value=" + a + ">" +
           label + "</button></form>";
  };
  if (upd) {
    h += btn("orig", "Palaa alkuperäiseen versioon", "Käynnistetäänkö alkuperäiseen versioon?");
    h += btn("origdel", "Palaa alkuperäiseen ja poista päivitys", "Palataanko alkuperäiseen ja poistetaan päivitys?");
    h += "<p class=note>Uuden päivityksen voi ladata vain alkuperäisestä versiosta: palaa ensin alkuperäiseen.</p>";
  } else {
    h += "<h2>Lataa päivitys</h2><form method=post action=/ohjelmisto/lataa enctype=multipart/form-data "
         "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('button').textContent="
         "'Ladataan… älä sulje sivua'\"><input type=file name=f accept=.bin required>"
         "<button>Lataa ja käynnistä</button></form>"
         "<p class=note>Tiedosto on arduino-cli:n kääntämä <b>kahvibotti.ino.bin</b>. Se korvaa aiemman päivityksen; "
         "alkuperäinen versio säilyy aina. Uusi versio on koeajalla, kunnes se on ollut verkossa ja lukenut vaakaa "
         "20 s (enintään 3 min): jos se kaatuu tai nollautuu sitä ennen, botti palaa itse alkuperäiseen ja poistaa "
         "rikkinäisen päivityksen. Älä avaa sarjaporttimonitoria sillä välin – sekin voi nollata laitteen. Lataus kestää noin 10–30 s, ja yhteys katkeaa hetkeksi uudelleenkäynnistyksen ajaksi.</p>";
    if (fw::hasFirmware(1)) {
      h += btn("upd", "Käynnistä päivitettyyn versioon", "Käynnistetäänkö päivitettyyn versioon?");
      h += btn("del", "Poista päivitys", "Poistetaanko päivitetty versio?");
    }
  }
  send("Ohjelmisto", h);
}

// ---------- login / logout ----------

int loginFails = 0;
int64_t loginBlockedUntil = 0;

inline void pageLogin() {
  String next = server.arg("next");
  // Only local pages (no open redirect): browsers read "//host", "/\host" and even "/<tab>/host" as another
  // site. The panel's own pages are plain paths, so allow nothing else.
  bool local = next.startsWith("/") && !next.startsWith("//");
  for (unsigned i = 0; local && i < next.length(); i++) local = isAlphaNumeric(next[i]) || strchr("/._-", next[i]);
  if (!local) next = "/huolto";
  String msg;
  if (server.method() == HTTP_POST) {
    if (nowMs() < loginBlockedUntil) {
      msg = "Liian monta väärää yritystä – odota minuutti ja yritä uudelleen.";
    } else {
      String u = server.arg("u"), pw = server.arg("p");
      bool owner = u == ADMIN_USER && pw == ADMIN_PASSWORD;
      bool panel = panelUser.length() && u == panelUser && pw == panelPass;
      if (owner || panel) {
        loginFails = 0;
        server.sendHeader("Set-Cookie", "kb=" + newSession(owner) + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=43200");
        backTo(next.c_str());
        return;
      }
      if (++loginFails >= 5) {  // password guessing: pause all logins for a minute
        loginFails = 0;
        loginBlockedUntil = nowMs() + 60000;
        xSemaphoreTake(coreLock, portMAX_DELAY);
        logEvent(nowMs(), "5 väärää kirjautumisyritystä: kirjautuminen tauolla 1 min", 0, 0);
        xSemaphoreGive(coreLock);
      }
      msg = "Väärä käyttäjätunnus tai salasana.";
    }
  }
  String h;
  if (msg.length()) h += "<p class=big style='color:#9b2c2c'>" + esc(msg) + "</p>";
  h += "<form method=post action='/kirjaudu?next=" + esc(next) + "'>"
       "<p>Käyttäjätunnus:<br><input name=u autocomplete=username required autofocus></p>"
       "<p>Salasana:<br><input name=p type=password autocomplete=current-password required></p>"
       "<button>Kirjaudu</button></form><p class=note>Ylläpitosivut (Huolto, Wi-Fi, Ohjelmisto) vaativat "
       "kirjautumisen. Info ja Tilastot näkyvät kaikille verkossa oleville.</p>";
  send("Ylläpito – kirjaudu", h);
}

inline void pageLogout() {
  Session* x = currentSession();
  if (x) x->tok = "";  // the cookie is worthless from now on, whatever the browser keeps
  server.sendHeader("Set-Cookie", "kb=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
  backTo("/");
}

inline void pageWifi() {
  if (!admin(true)) return;
  if (server.method() == HTTP_POST) {
    if (server.hasArg("del")) {
      if (!wifisetup::deleteSlot(server.arg("del").toInt())) wifisetup::status = "Käytössä olevaa verkkoa ei voi poistaa.";
    } else if (server.arg("s").length()) {
      wifisetup::tryNew(server.arg("s"), server.arg("p"));
    }
    backTo("/wifi");
    return;
  }
  if (!wifisetup::portalOn) {
    wifisetup::scan();  // on the hotspot this was done when it opened: scanning now would drop the phone
  }
  String b;
  if (wifisetup::status.length()) b += "<p class=big>" + esc(wifisetup::status) + "</p>";
  b += "<table>";
  for (int i = 0; i < wifisetup::netCount; i++) {
    const wifisetup::Net& n = wifisetup::nets[i];
    String right;
    if (wifisetup::inUse(n.ssid)) right = "<b>✓ käytössä</b>";
    else if (n.slot >= 0)
      right = "<form method=post action=/wifi style=margin:0><button name=del value=" + String(n.slot) +
              " style='width:auto;margin:0;padding:.2em .8em'>Poista</button></form>";
    b += row(esc(n.ssid) + (n.slot < 0 ? "<br><span class=note>sisäänrakennettu</span>" : ""), right);
  }
  b += "</table>";
  if (wifisetup::savedCount() >= wifisetup::MAX_SAVED)
    b += "<p class=note>Tallennettuja verkkoja on " + String(wifisetup::MAX_SAVED) +
         " (enimmäismäärä). Uuden lisääminen vaatii yhden poistamista; olemassa olevan salasanan voi vaihtaa.</p>";
  b += "<h2>Lisää tai vaihda verkko</h2><form method=post action=/wifi><p>Verkon nimi:<br><input name=s list=n required>"
       "<datalist id=n>" + wifisetup::scanned + "</datalist></p><p>Verkon salasana:<br><input name=p type=password></p>"
       "<button>Tallenna ja yhdistä</button></form>"
       "<p class=note>Uusi verkko tallennetaan vasta, kun yhteys siihen onnistuu. Sisäänrakennettu verkko "
       "(secrets.h) on aina varalla eikä sitä voi poistaa. Jos vaihdat verkkoa tämän sivun kautta, "
       "yhteys tähän sivuun katkeaa hetkeksi.</p>";
  send("Wi-Fi", b);
}

inline void begin() {
  server.on("/", pageTila);
  server.on("/wifi", pageWifi);
  server.on("/tilastot", pageStats);
  server.on("/tilastot.csv", pageCsv);
  server.on("/huolto", pageHuolto);
  server.on("/huolto/live", pageLive);
  server.on("/ohjelmisto", pageFirmware);                          // page + buttons: no upload callback
  server.on("/ohjelmisto/lataa", HTTP_POST, uploadDone, onUpload);  // the only route with one
  server.on("/kirjaudu", pageLogin);
  server.on("/ulos", pageLogout);
  static const char* hdrs[] = {"Content-Type", "Cookie"};  // onUpload checks Content-Type; sessions use Cookie
  server.collectHeaders(hdrs, 2);
  server.onNotFound([] {
    if (viaHotspot()) {  // captive portal: every URL a phone probes leads to the Wi-Fi page
      server.sendHeader("Location", "http://192.168.4.1/wifi");
      server.send(302);
    } else {
      server.send(404, "text/plain", "Ei löydy");
    }
  });
  loadPanelAccount();
  server.begin();
}

inline void loop() {
  server.handleClient();
  if (restartAt && nowMs() > restartAt) {
    // Not in the middle of a brew, or with brews still waiting to be saved (RAM only): they'd be lost.
    // Wait for it (a brew is over in ~10 min), but at most 20 min. The panel keeps working meanwhile.
    static int64_t waitingSince = 0;
    xSemaphoreTake(coreLock, portMAX_DELAY);
    bool busy = core.brewing || core.flowing || pendingN;
    if (busy && !waitingSince) logEvent(nowMs(), "Uudelleenkäynnistys odottaa, kunnes keitto on valmis", 0, 0);
    xSemaphoreGive(coreLock);
    if (busy && !waitingSince) waitingSince = nowMs();
    if (!busy || nowMs() - waitingSince > 20 * 60000) {
      if (!restartWhy) restartWhy = fw::PLANNED_PANEL;
      fw::notePlanned(restartWhy);
      // A deliberate restart means this firmware works well enough to serve the panel, so don't let the
      // bootloader treat it as a failed update. Switching back to the original sets the boot slot itself.
      if (esp_ota_get_boot_partition() == esp_ota_get_running_partition()) fw::markValid();
      ESP.restart();
    }
  }
  static bool mdns = false;
  if (!mdns && WiFi.status() == WL_CONNECTED && MDNS.begin("emukahvibotti")) {
    MDNS.addService("http", "tcp", 80);
    mdns = true;
    Serial.printf(">>> admin panel: http://emukahvibotti.local or http://%s\n", WiFi.localIP().toString().c_str());
  }
}

}  // namespace admin
