// Wi-Fi with a fallback setup hotspot. Include once, from kahvibotti.ino, after secrets.h.
// The web pages (including the Wi-Fi form) live in admin.h; this file only connects.
//
// Known networks: up to MAX_SAVED saved from the admin page (NVS, saved only after the bot has
// actually connected; deletable, except the one in use) + the one in secrets.h ("built-in":
// compiled in, can't be deleted, the permanent fallback).
// Offline 5 min -> opens "EmuKahviBottiHotspot" (password: see portalPass()); a phone joining it lands on
// the admin panel's Wi-Fi page. Meanwhile the bot keeps retrying the known networks every 2 min
// (paused while a phone is on the hotspot, since a retry hops channels and would drop the phone,
// but at most 10 min), so a Wi-Fi outage that gets fixed heals itself and the hotspot closes.
#pragma once
// Hotspot password: the panel admin's (NVS "admin"/"pass", set on the Huolto page) once set, otherwise
// the owner's ADMIN_PASSWORD. WPA2 needs 8-63 characters (both are validated for that).
inline String portalPass() {
  Preferences p;
  p.begin("admin", true);
  String pw = p.getString("pass", "");
  p.end();
  return pw.length() >= 8 ? pw : String(ADMIN_PASSWORD);
}
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>

namespace wifisetup {

constexpr int64_t PORTAL_AFTER_MS = 5 * 60000;  // offline this long -> open the setup hotspot
constexpr int64_t RETRY_MS = 20000;             // try the next known network (hotspot closed)
constexpr int64_t RETRY_PORTAL_MS = 120000;     // ...and while the hotspot is open
constexpr int64_t CANDIDATE_MS = 20000;         // time a network entered on the page gets to connect
const char* AP_NAME = "EmuKahviBottiHotspot";

constexpr int MAX_SAVED = 3;
struct Net { String ssid, pass; int slot; };  // slot -1 = built-in (secrets.h)
Net nets[MAX_SAVED + 1];
int netCount = 0, tryIdx = 0;
int64_t offlineSince = 0, tryStarted = -RETRY_MS, candStarted = 0;
bool portalOn = false, candPending = false;
String candSsid, candPass, status, scanned;  // status + scanned are shown on the Wi-Fi page
DNSServer dns;

inline int64_t ms() { return esp_timer_get_time() / 1000; }

inline String key(char k, int slot) { return String(k) + slot; }  // "s0", "p0", ...

inline void loadNets() {
  Preferences p;
  p.begin("wifi", false);
  if (p.isKey("ssid")) {  // one-time migration from the single-network version
    if (!p.isKey("s0")) { p.putString("s0", p.getString("ssid", "")); p.putString("p0", p.getString("pass", "")); }
    p.remove("ssid");
    p.remove("pass");
  }
  netCount = 0;
  bool builtinSaved = false;
  for (int i = 0; i < MAX_SAVED; i++) {
    String s = p.getString(key('s', i).c_str(), "");
    if (!s.length()) continue;
    nets[netCount++] = {s, p.getString(key('p', i).c_str(), ""), i};
    if (s == WIFI_SSID) builtinSaved = true;  // same name saved (e.g. new password): the saved one wins
  }
  p.end();
  if (!builtinSaved) nets[netCount++] = {WIFI_SSID, WIFI_PASSWORD, -1};
  tryIdx = 0;
}

inline int savedCount() {
  int n = 0;
  for (int i = 0; i < netCount; i++) if (nets[i].slot >= 0) n++;
  return n;
}

inline bool isSaved(const String& ssid) {
  for (int i = 0; i < netCount; i++) if (nets[i].slot >= 0 && nets[i].ssid == ssid) return true;
  return false;
}

inline bool inUse(const String& ssid) { return WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid; }

// Saves into the network's own slot if it's already saved (password change), else the first free one.
// False if every slot is taken (possible for the built-in network with a new password: tryNew lets it through).
inline bool saveNet(const String& ssid, const String& pass) {
  int slot = -1;
  for (int i = 0; i < netCount; i++) if (nets[i].slot >= 0 && nets[i].ssid == ssid) slot = nets[i].slot;
  Preferences p;
  p.begin("wifi", false);
  for (int i = 0; slot < 0 && i < MAX_SAVED; i++)
    if (!p.getString(key('s', i).c_str(), "").length()) slot = i;
  if (slot >= 0) {  // password first: the name marks the slot as used, so a power cut in between leaves no
                    // network with a missing password
    p.putString(key('p', slot).c_str(), pass);
    p.putString(key('s', slot).c_str(), ssid);
  }
  p.end();
  loadNets();
  return slot >= 0;
}

// From the Wi-Fi page. Refuses the network in use (and the built-in one, which has no slot).
inline bool deleteSlot(int slot) {
  for (int i = 0; i < netCount; i++) {
    if (nets[i].slot != slot || slot < 0) continue;
    if (inUse(nets[i].ssid)) return false;
    Preferences p;
    p.begin("wifi", false);
    p.remove(key('s', slot).c_str());
    p.remove(key('p', slot).c_str());
    p.end();
    status = "Verkko " + nets[i].ssid + " poistettu.";
    loadNets();
    return true;
  }
  return false;
}

inline void connectTo(const String& ssid, const String& pass) {
  WiFi.disconnect();  // cancel any attempt still in progress, or begin() is rejected
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf(">>> Wi-Fi: trying '%s'\n", ssid.c_str());
}

// From the Wi-Fi page: try a new network. Saved only if it connects (see loop()).
// Returns false if all MAX_SAVED slots are taken by other networks.
inline bool tryNew(const String& ssid, const String& pass) {
  if (!isSaved(ssid) && ssid != WIFI_SSID && savedCount() >= MAX_SAVED) {
    status = "Tallennettuja verkkoja on jo " + String(MAX_SAVED) + " – poista ensin yksi.";
    return false;
  }
  candSsid = ssid;
  candPass = pass;
  candPending = true;
  candStarted = ms();
  status = "Yhdistetään verkkoon " + ssid + "… Jos yhteys onnistuu, laite siirtyy siihen. "
           "Muuten vanhat verkot jäävät voimaan.";
  connectTo(ssid, pass);
  return true;
}

// Names for the suggestion list on the Wi-Fi page (a few seconds).
inline void scan() {
  int n = WiFi.scanNetworks();
  scanned = "";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    s.replace("&", "&amp;"); s.replace("\"", "&quot;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
    scanned += "<option value=\"" + s + "\">";
  }
  WiFi.scanDelete();
}

inline void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  scan();
  WiFi.softAP(AP_NAME, portalPass().c_str());
  dns.start(53, "*", WiFi.softAPIP());  // every name -> us, so phones pop the page up by themselves
  portalOn = true;
  status = "";
  Serial.printf(">>> Wi-Fi offline 5 min: setup hotspot '%s' open at 192.168.4.1\n", AP_NAME);
}

inline void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalOn = false;
  Serial.println(">>> setup hotspot closed");
}

inline void begin() {
  if (strlen(ADMIN_PASSWORD) < 8) Serial.println("!!! ADMIN_PASSWORD in secrets.h must be 8+ characters (it can be the hotspot password)");
  loadNets();
  WiFi.mode(WIFI_STA);
  // Our own loop does all reconnecting. The driver's auto-reconnect keeps retrying the *last*
  // network and meanwhile rejects every other one ("sta is connecting, cannot set config"), which
  // left the bot stuck on a vanished network in the 2026-09-24 hardware test.
  WiFi.setAutoReconnect(false);
}

// Call from loop(). Returns true when connected.
inline bool loop() {
  int64_t now = ms();
  if (portalOn) dns.processNextRequest();
  if (WiFi.status() == WL_CONNECTED) {
    if (candPending && WiFi.SSID() == candSsid) {  // the new network works: only now save it
      bool saved = (candSsid == WIFI_SSID && candPass == WIFI_PASSWORD) || saveNet(candSsid, candPass);
      status = saved ? "Verkko " + candSsid + " tallennettu."
                     : "Verkko " + candSsid + " toimii, mutta sitä ei voitu tallentaa: tallennettuja verkkoja on jo " +
                       String(MAX_SAVED) + " – poista ensin yksi ja lisää verkko uudelleen.";
      Serial.printf(">>> new Wi-Fi '%s' %s\n", candSsid.c_str(), saved ? "saved" : "NOT saved (all slots taken)");
    }
    candPending = false;
    if (portalOn) stopPortal();
    offlineSince = 0;
    for (int i = 0; i < netCount; i++)  // after a drop, retry the network we were on first
      if (nets[i].ssid == WiFi.SSID()) tryIdx = i;
    return true;
  }
  if (offlineSince == 0) offlineSince = now;
  if (!portalOn && now - offlineSince > PORTAL_AFTER_MS) startPortal();

  if (candPending) {
    if (now - candStarted < CANDIDATE_MS) return false;
    candPending = false;
    status = "Yhteys verkkoon " + candSsid + " ei onnistunut – tarkista nimi ja salasana. Vanhat verkot ovat tallessa.";
    Serial.printf(">>> Wi-Fi '%s' from the admin page failed, not saved\n", candSsid.c_str());
  }
  // Pause retries while a phone is on the hotspot, but at most 10 min: a phone that auto-joined
  // and lingers must not block the old network from ever coming back.
  bool phoneOnHotspot = portalOn && WiFi.softAPgetStationNum() > 0;
  int64_t since = now - tryStarted;
  if ((!phoneOnHotspot && since > (portalOn ? RETRY_PORTAL_MS : RETRY_MS)) || since > 10 * 60000) {
    connectTo(nets[tryIdx].ssid, nets[tryIdx].pass);
    tryIdx = (tryIdx + 1) % netCount;
    tryStarted = now;
  }
  return false;
}

}  // namespace wifisetup
