// Kahvibotti core: turns tared load-cell samples into "what should /kahvi say".
// Plain C++ (no Arduino), so the same code runs on the ESP32 and in test/replay_test.cpp,
// which replays the labelled calibration logs. All constants come from the 2026-09-23/24
// calibration runs, see the design notes "Results" sections.
//
// Units: rawT = L + R in raw tared counts. A positive step = mass removed from the platform.
// Feed one sample per ~1 s (the logic was tuned on ~0.87 Hz logs), time in ms, monotonic 64-bit.
//
// Robustness, by design: only *changes* between stable levels are used, never the absolute zero,
// so power-on warm-up drift and slow drift over hours (measured: <= ~1.5 g/h) are simply tracked.
// Every state belief self-corrects at the next carafe event. See the design notes "Failsafes".
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

namespace kahvi {

// --- calibration (counts) ---
// The values that depend on how the platform's load cells sit. After a cell moved (2026-09-27) the carafe
// read 17 % lighter and its left/right pattern flipped sign, while the basket, reservoir and machine steps
// stayed put, so these are a set: CALIB_S is the device's (run S), CALIB_A_R the geometry the labelled logs
// A-R were recorded with (the replay and soak tests use it for those).
struct Calib {
  float potPerG;      // carafe, counts per gram of coffee
  float emptyCarafe;  // empty carafe lift/return step
  float carafeMargin; // carafe seating error allowance
  float moveLPerG;    // L change per gram moved reservoir -> right side while brewing
  float machineDLdT;  // whole-machine steps have dL/dT above this; carafe lifts below
};
// Run S (2026-09-27): empty carafe 73.8-75.7k; full carafe 213k - 75k = 138k for ~660 g of coffee (750 g
// water - 73 g held by 42 g grounds - steam; 6,8 dl poured) = 210/g; 98.8k of L for 750 g moved = 132/g;
// carafe dL/dT +0.24..+0.28, machine +0.58: threshold between them. Margin: the old 22k seating error x 0.83.
constexpr Calib CALIB_S = {210, 74500, 21000, 132, 0.42f};
// Runs A-R (2026-09-21..25): empty carafe 84-94k, 252/g (A15 water reference), 214/g moved, carafe dL/dT
// -0.29 vs machine +0.22..+0.41, seating error up to 22k.
constexpr Calib CALIB_A_R = {252, 90000, 25000, 214, 0.1f};

constexpr float RES_PER_G = 213;        // reservoir fill, counts per gram (194 at 250 g, 213 from 750 g; run S ~217)
constexpr float CUP_G = 105;            // 1 kuppi = 105 g coffee (125 ml water minus retention and steam)
constexpr float YIELD = 0.84f;          // coffee in pot / water brewed (= CUP_G / 125)
constexpr float BASKET_MAX = 100000;    // heaviest basket step seen: 90.9k (70 g grounds, wet)
constexpr float STEP_MIN = 6000;        // smaller stable-to-stable changes are creep, tracked silently
constexpr float STABLE_SPREAD = 2000;   // 3 consecutive samples within this = a stable level
constexpr float BIG_STEP = 20000;       // right-side object steps (carafe, basket) are all above this
constexpr float MACHINE_STEP = 400000;  // whole machine off/on: 530-692k measured (runs K, S); a brim-full
                                        // carafe lift is up to ~417k (A-R) / ~310k (S): see machineDLdT
constexpr float IGNORE_STEP = 600000;   // larger without the machine's pattern: someone leaning hard, ignore
constexpr int64_t LEAN_PAIR_MS = 120000;  // a machine-sized "lift" this soon after an equal set-down = a lean released
constexpr float POT_MAX_G = 1300;       // a 10-cup pot holds ~1000-1130 g
constexpr float WATER_MAX_G = 1500;     // sanity cap. Reservoir brim measured 1375 g = 13.75 dl ≈ 11 cups (run L)
constexpr float FLOW_L_RATE = 300;      // counts/s. Brewing moves L up ~800-900/s and R down alike
constexpr int64_t FLOW_WINDOW_MS = 10000;
constexpr int64_t FLOW_CONFIRM_MS = 8000;   // flow must look like brewing this long before it counts
constexpr int64_t FLOW_END_MS = 15000;      // flow must be absent this long before the brew counts as drained
constexpr int64_t FLOW_MAX_MS = 15 * 60000; // a 10-cup pump phase is ~5 min; longer = something's wrong
constexpr float FLOW_MIN_G = 50;            // a "brew" that moved less than this was a false start
constexpr float DEFAULT_RATE_GPS = 4.2f;
constexpr int64_t AWAY_LONG_MS = 15 * 60000;  // carafe gone this long = washing, or a misread
constexpr int64_t FEED_TIMEOUT_MS = 30000;    // no good sample this long = the scale isn't answering

// Drip tail after the reservoir empties, measured: 2/4/6/10 cups; 8 cups interpolated.
inline float tailSeconds(float waterG) {
  static const float w[] = {250, 500, 750, 1000, 1250};
  static const float s[] = {92, 90, 138, 184, 231};
  if (waterG <= w[0]) return s[0];
  for (int i = 1; i < 5; i++)
    if (waterG <= w[i]) return s[i - 1] + (s[i] - s[i - 1]) * (waterG - w[i - 1]) / (w[i] - w[i - 1]);
  return s[4];
}

enum Kind { EMPTY, CUPS, AWAY, GONE, BREWING, FAULT, MACHINE_AWAY };

struct Reply {
  Kind kind;
  int cups;          // CUPS: band 2..10. BREWING: cups being brewed, 0 = unknown
  int etaMin;        // BREWING: minutes left, 0 = under a minute, -1 = unknown
  int64_t brewedAt;  // ms timestamp of the last finished brew, 0 = unknown
};

inline int band(float cups) {  // 1-3 -> 2, 3-5 -> 4, ... >= 9 -> 10
  int b = 2 * (int)floorf((cups + 1) / 2);
  return b < 2 ? 2 : (b > 10 ? 10 : b);
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Core {
  Calib cal = CALIB_S;      // the platform geometry (tests set CALIB_A_R for the old logs)
  // what the bot believes. Boot assumes the "ready" state: carafe and basket on, pot empty.
  bool carafeOn = true, basketOn = true, machineAway = false;
  float potG = 0;           // coffee in the carafe, grams
  float fillG = 0;          // water poured into the reservoir since the last brew (may go <0 from a lid)
  bool brewing = false, flowing = false, carafeTouched = false;
  int64_t awaySince = 0, lastFeed = 0;
  int64_t flowCand = -1, candT0 = 0;
  float candL0 = 0;
  int64_t flowStart = 0, flowLastSeen = 0, flowEnd = 0, doneAt = 0, brewedAt = 0;
  uint32_t freshStarts = 0;  // how many freshStart()s: tells the device to forget its stored brew time
  float brewWaterG = 0, potBeforeBrew = 0, L0 = 0, movedG = 0, movedAtReturn = 0;
  // Drip-stop: while the carafe is away mid-brew, the basket holds the coffee back and releases it after
  // the return. The scale can't see that (basket and carafe are both on the right side), so a return
  // reading is too low. Instead: coffee in the carafe = everything this brew makes (brewExpected) minus what
  // was poured during it (reading at lift - reading at return), never less than what was measured.
  float brewExpected = -1, pouredInBrew = 0, potAtLift = 0;
  bool liftedInBrew = false;
  int64_t etaDone = 0;      // estimated finish while flowing, smoothed (see trackFlow)
  int64_t leanAt = -1;
  float leanSize = 0;

  // Optional: called on every decision (for the admin panel's event log). what = a Finnish label,
  // a/b = the numbers that decision was based on. Runs inside feed(); keep it quick.
  void (*onEvent)(int64_t t, const char* what, float a, float b) = nullptr;
  int64_t evT = 0;  // time of the sample being processed, for events raised deep inside
  void ev(const char* what, float a = 0, float b = 0) { if (onEvent) onEvent(evT, what, a, b); }

  // stable-level tracker
  float s[3][3];  // last 3 samples: T, L, R
  int n = 0;
  bool haveRef = false;
  float refT = 0, refL = 0, refR = 0;
  // flow window
  static constexpr int W = 32;
  int64_t wt[W];
  float wl[W], wr[W], wT[W];
  int wn = 0;

  void feed(int64_t t, float L, float R) {
    lastFeed = t;
    evT = t;
    float T = L + R;
    for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) s[i][j] = s[i + 1][j];
    s[2][0] = T; s[2][1] = L; s[2][2] = R;
    if (n < 3) n++;
    pushWindow(t, T, L, R);
    if (n == 3) trackLevel(t);
    trackFlow(t, L);
  }

  Reply reply(int64_t t) const {
    Reply r{EMPTY, 0, -1, brewedAt};
    if (lastFeed == 0 || t - lastFeed > FEED_TIMEOUT_MS) { r.kind = FAULT; return r; }
    if (machineAway) { r.kind = MACHINE_AWAY; return r; }
    if (brewing && t < doneAt) {
      r.kind = BREWING;
      if (brewWaterG > 100) {
        int c = 2 * (int)lroundf(brewWaterG / 250);
        r.cups = c < 2 ? 2 : (c > 10 ? 10 : c);
        float left = ((flowing ? etaDone : doneAt) - t) / 1000.0f;
        r.etaMin = left < 60 ? 0 : (int)lroundf(left / 60);
      }
      return r;
    }
    if (!carafeOn) { r.kind = t - awaySince > AWAY_LONG_MS ? GONE : AWAY; return r; }
    float cups = potG / CUP_G;
    if (cups < 1) return r;
    r.kind = CUPS;
    r.cups = band(cups);
    return r;
  }

 private:
  void pushWindow(int64_t t, float T, float L, float R) {
    if (wn == W) { for (int i = 1; i < W; i++) { wt[i-1]=wt[i]; wl[i-1]=wl[i]; wr[i-1]=wr[i]; wT[i-1]=wT[i]; } wn--; }
    wt[wn] = t; wl[wn] = L; wr[wn] = R; wT[wn] = T; wn++;
  }

  void trackLevel(int64_t t) {
    float lo = s[0][0], hi = s[0][0];
    for (int i = 1; i < 3; i++) { lo = fminf(lo, s[i][0]); hi = fmaxf(hi, s[i][0]); }
    if (hi - lo > STABLE_SPREAD) return;  // hands on the machine, pouring, glitches: wait
    float T = med(0), L = med(1), R = med(2);
    if (!haveRef) { haveRef = true; refT = T; refL = L; refR = R; return; }
    float dT = T - refT, dL = L - refL;
    refT = T; refL = L; refR = R;  // tracks creep too
    if (fabsf(dT) >= STEP_MIN) onStep(t, dT, dL);
  }

  float med(int j) const {
    float a = s[0][j], b = s[1][j], c = s[2][j];
    return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
  }

  void carafeOff(int64_t t, float dT) {
    carafeOn = false; carafeTouched = true; awaySince = t;
    potG = clampf((dT - cal.emptyCarafe) / cal.potPerG, 0, POT_MAX_G);
    if (brewing) { liftedInBrew = true; potAtLift = potG; }  // the basket keeps dripping into itself now
    ev("Lasikannu nostettu (g kahvia, askel)", potG, dT);
  }
  void carafeBack(float a) {
    carafeOn = true; carafeTouched = true;
    potG = clampf((a - cal.emptyCarafe) / cal.potPerG, 0, POT_MAX_G);
    movedAtReturn = movedG;
    if (liftedInBrew) {  // lifted while this brew was still dripping (even if it's back only now)
      liftedInBrew = false;
      pouredInBrew += fmaxf(0, potAtLift - potG);
      if (brewExpected >= 0) potG = fmaxf(potG, clampf(brewExpected - pouredInBrew, 0, POT_MAX_G));
    }
    ev("Lasikannu palautettu (g kahvia, askel)", potG, -a);
  }

  // Whole machine taken off the platform, or put back. While away, /kahvi says so. On return,
  // start fresh ("ready, pot empty"): the machine may come back in any state, and the first
  // carafe lift re-measures it (run K: returned with 250 g, lift measured 259 g).
  void onMachineStep(int64_t t, float dT) {
    if (dT > 0) {  // lifted off
      if (leanAt >= 0 && t - leanAt < LEAN_PAIR_MS && fabsf(dT - leanSize) < 0.2f * leanSize) {
        leanAt = -1;  // just someone who leaned on it letting go
        ev("Nojaaminen päättyi (askel)", dT);
        return;
      }
      machineAway = true;
      brewing = flowing = false;
      ev("Kahvinkeitin nostettu pois (askel)", dT);
    } else if (machineAway) {  // put back
      freshStart();
      ev("Kahvinkeitin palautettu, tila nollattu (askel)", dT);
    } else {  // a machine-sized press on a machine that's there: someone leaning on it
      leanAt = t;
      leanSize = -dT;
      ev("Joku nojaa keittimeen (askel)", dT);
    }
  }

 public:
  void freshStart() {
    machineAway = false;
    carafeOn = basketOn = true;
    potG = fillG = 0;
    brewing = flowing = carafeTouched = false;
    brewExpected = -1; pouredInBrew = 0; liftedInBrew = false;
    brewedAt = 0;  // any old brew time no longer applies
    freshStarts++;
  }

 private:
  void onStep(int64_t t, float dT, float dL) {
    if (fabsf(dT) >= MACHINE_STEP && dL / dT > cal.machineDLdT) { onMachineStep(t, dT); return; }
    if (machineAway) return;               // empty platform: nothing else can happen until it's back
    if (fabsf(dT) > IGNORE_STEP) { ev("Iso tuntematon askel ohitettu", dT); return; }  // hard lean
    // Reservoir: water or its lid. L-dominant (dL/dT ~ +0.67). Carafe/basket have dL/dT ~ -0.2..-0.3.
    // Not while flowing: the reservoir can't be filled mid-brew, and a lift mid-brew is R-dominant.
    if (dL / dT >= 0.4f) {
      if (!flowing) {
        fillG = clampf(fillG - dT / RES_PER_G, -200, WATER_MAX_G);
        ev("Vesisäiliö: vettä tai kansi (g muutos, g yhteensä)", -dT / RES_PER_G, fillG);
      }
      return;
    }
    // A carafe/basket step while the brew flows also moves L (a lift: dL = -0.3 dT). Shift the flow's
    // L baseline by it, so "water moved" (from L) doesn't count the step as water.
    if (brewing) L0 += dL;  // the whole brew: the flow may resume after a pause (test 15)
    if (fabsf(dT) < BIG_STEP) return;  // basket cover, grounds added, bumps
    // Size decides first: the empty carafe alone is 84-94k (A-R) / ~75k (S), so anything under CARAFE_MIN
    // is the basket or just its paper + grounds (lifted in or out with the basket left in place: 13-53k).
    // Only the band up to BASKET_MAX (an almost empty carafe vs a full basket) needs the beliefs.
    const float CARAFE_MIN = cal.emptyCarafe - cal.carafeMargin;
    if (dT > 0) {  // something lifted off the right side
      if (dT < CARAFE_MIN) {
        basketOn = false;  // basket, or its contents
        ev("Suodatinsuppilo, kansi tai porot pois (askel)", dT);
      } else if (dT < BASKET_MAX && basketOn &&
                 (!carafeOn || dT < cal.emptyCarafe + potG * cal.potPerG - cal.carafeMargin)) {
        basketOn = false;  // a full basket; a carafe with this much coffee would be heavier
        ev("Täysi suodatinsuppilo pois (askel)", dT);
      } else {
        // the carafe. If it was believed away, its return was missed (e.g. during a power cut)
        carafeOff(t, dT);
      }
    } else {  // something set down on the right side
      float a = -dT;
      if (a < CARAFE_MIN) {
        basketOn = true;  // basket back, or fresh paper + grounds put in
        ev("Suodatinsuppilo, kansi tai porot paikalleen (askel)", dT);
      } else if (!carafeOn || basketOn || a >= BASKET_MAX) {
        carafeBack(a);  // if it was believed on, its lift was missed
      } else {
        basketOn = true;  // a full wet basket put back while the carafe stays on
        ev("Täysi suodatinsuppilo paikalleen (askel)", dT);
      }
    }
  }

  void trackFlow(int64_t t, float L) {
    // slope over the oldest sample still within FLOW_WINDOW_MS
    int i = 0;
    while (i < wn - 1 && t - wt[i] > FLOW_WINDOW_MS) i++;
    float dt = (t - wt[i]) / 1000.0f;
    bool flow = false;
    if (dt >= 5) {
      float sL = (L - wl[i]) / dt, sR = (wr[wn - 1] - wr[i]) / dt, sT = (wT[wn - 1] - wT[i]) / dt;
      flow = sL > FLOW_L_RATE && sR < -FLOW_L_RATE && fabsf(sT) < 0.3f * sL;
    }
    if (!flow) flowCand = -1;
    else if (!flowing && flowCand < 0) { flowCand = t; candT0 = wt[i]; candL0 = wl[i]; }
    if (flow && !flowing && t - flowCand >= FLOW_CONFIRM_MS) {
      if (machineAway) freshStart();  // brewing on the platform: the machine is evidently back
      flowing = true;
      doneAt = INT64_MAX;
      if (!brewing) {  // a new brew starts
        brewing = true; carafeTouched = false;
        flowStart = candT0; L0 = candL0; movedAtReturn = 0; etaDone = 0;
        brewExpected = -1; pouredInBrew = 0; liftedInBrew = false;
        potBeforeBrew = potG;
        brewWaterG = fillG;  // <=100 g means the fill wasn't seen
        ev("Keitto alkoi (g vettä vesisäiliössä)", brewWaterG);
      }
    }
    if (flow && flowing) flowLastSeen = t;
    if (flowing) {
      // Only while the flow pattern is actually seen: a lift/return disturbs L for a few seconds, and the
      // flow may end on that very sample (drip-stop test 14 caught it reading 672 g instead of 1010 g).
      if (flow) movedG = (L - L0) / cal.moveLPerG;
      // A fill misread as smaller (e.g. a lid step near the reservoir threshold) can't be less
      // than what has already left the reservoir.
      if (brewWaterG > 100 && movedG > brewWaterG) brewWaterG = movedG;
      // ETA: remaining water at this brew's own rate + drip tail. The flow comes in surges, so the
      // finish time may only move earlier, or later by more than a minute (really slower); that
      // stops the countdown bouncing 3 -> 4 -> 3 min.
      float el = (t - flowStart) / 1000.0f;
      float rate = (el > 20 && movedG > 20) ? movedG / el : DEFAULT_RATE_GPS;
      float rem = brewWaterG - movedG;
      int64_t done = t + (int64_t)(((rem > 0 ? rem / rate : 0) + tailSeconds(brewWaterG)) * 1000);
      if (etaDone == 0 || done < etaDone || done > etaDone + 60000) etaDone = done;
      bool drained = t - flowLastSeen > FLOW_END_MS;
      if (drained || t - flowStart > FLOW_MAX_MS) {
        flowing = false;
        if (movedG < FLOW_MIN_G) { brewing = false; ev("Väärä keittohälytys peruttu (g)", movedG); return; }
        flowEnd = drained ? flowLastSeen : t;
        // water: the fill we saw, unless it disagrees with what actually moved (fill missed,
        // or an old fill that never got brewed); the L-based figure is within ~12 %
        float water = movedG;
        if (brewWaterG > 100 && fabsf(brewWaterG - movedG) < 0.3f * movedG) water = brewWaterG;
        brewWaterG = water;
        doneAt = flowEnd + (int64_t)(tailSeconds(water) * 1000);
        brewExpected = clampf(potBeforeBrew + water * YIELD, 0, POT_MAX_G);
        if (!carafeTouched) potG = brewExpected;
        else if (carafeOn) potG = fmaxf(potG, clampf(brewExpected - pouredInBrew, 0, POT_MAX_G));  // drip-stop
        fillG = 0;
        ev("Vesisäiliö tyhjä (g vettä, g kahvia)", water, potG);
      }
    }
    if (brewing && !flowing && t >= doneAt) { brewing = false; brewedAt = doneAt; ev("Kahvi valmis (g kahvia)", potG); }
  }
};

// Finnish reply text. brewed = wall-clock time of reply.brewedAt (nullptr if unknown),
// today = brewed on the same calendar day as now.
inline void format(const Reply& r, const struct tm* brewed, bool today, char* out, size_t n) {
  int k = 0;
  switch (r.kind) {
    case BREWING:
      k = snprintf(out, n, "Kahvi keittymässä");
      if (r.cups) k += snprintf(out + k, n - k, " – n. %d kuppia", r.cups);
      if (r.etaMin == 0) snprintf(out + k, n - k, ", valmis alle minuutissa");
      else if (r.etaMin > 0) snprintf(out + k, n - k, ", valmis ~%d min", r.etaMin);
      break;
    case AWAY:
      snprintf(out, n, "Pannu on jonkun kädessä – kysy hetken päästä uudelleen.");
      break;
    case GONE:
      snprintf(out, n, "Pannu ei ole paikallaan.");
      break;
    case MACHINE_AWAY:
      snprintf(out, n, "Kahvinkeitin ei ole paikallaan.");
      break;
    case FAULT:
      snprintf(out, n, "Vaaka ei vastaa – en tiedä kahvitilannetta.");
      break;
    case EMPTY:
      snprintf(out, n, "Tyhjä");
      break;
    case CUPS:
      k = snprintf(out, n, "%d kuppia", r.cups);
      if (brewed && today) snprintf(out + k, n - k, " (keitetty klo %d.%02d)", brewed->tm_hour, brewed->tm_min);
      else if (brewed) snprintf(out + k, n - k, " (keitetty %d.%d. klo %d.%02d)", brewed->tm_mday, brewed->tm_mon + 1, brewed->tm_hour, brewed->tm_min);
      break;
  }
}

// /kahvi spam guard. Each reply is an HTTPS request (1-3 s on the C3) and Telegram allows a bot about
// 1 message/s per chat and 20/min per group (429 errors beyond). So: one reply per chat per 10 s (the
// previous answer is still on screen), and at most 12 replies per minute across all chats (the bot answers
// in every group and private chat it is in). Anything over that is ignored silently.
struct ReplyLimiter {
  static constexpr int CHATS = 8, PER_MIN = 12;
  static constexpr int64_t CHAT_GAP_MS = 10000;
  int64_t chat[CHATS] = {}, lastMs[CHATS] = {}, sent[PER_MIN] = {};
  int head = 0;  // sent[] is a ring of the last PER_MIN reply times; sent[head] is the oldest
  bool allow(int64_t chatId, int64_t now) {  // now = when the message was sent (ms), not when it's handled
    int slot = -1, oldest = 0;
    for (int i = 0; i < CHATS; i++) {
      if (lastMs[i] && chat[i] == chatId) slot = i;
      if (lastMs[i] < lastMs[oldest]) oldest = i;
    }
    if (slot >= 0 && now - lastMs[slot] < CHAT_GAP_MS) return false;
    if (sent[head] && now - sent[head] < 60000) return false;
    if (slot < 0) slot = oldest;  // forget the chat answered longest ago
    chat[slot] = chatId;
    lastMs[slot] = now;
    sent[head] = now;
    head = (head + 1) % PER_MIN;
    return true;
  }
};

}  // namespace kahvi
