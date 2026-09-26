// Replays the labelled calibration logs (test-data/) through kahvi::Core and checks what /kahvi would say
// shortly after each mark. Build and run from the repo root:
//   g++ -std=c++17 -O2 -o /tmp/replay firmware/kahvibotti/test/replay_test.cpp && /tmp/replay
// Expected codes: E = Tyhjä, A = carafe away, M = whole machine away, B = brewing, 2..10 = cup band. "a|b" = either is OK
// (borderline cases, the truth sits on a band edge). "-" = don't check.
// Timing-driven alternatives: X7 "first drip" = B|E (a brew is confirmed ~15 s after flow starts);
// X15 "remaining measured" = A|E (the user often put the carafe back before typing the mark);
// D11 = A|8 (mark typed before the carafe was set down; D12/D13 check the 8).
#include "../kahvi_core.h"
#include <cstring>
#include <string>
#include <vector>
#include <map>

struct Mark { int64_t t; std::string label; };
struct Sample { int64_t t; float L, R; };

static bool load(const char* name, std::vector<Sample>& S, std::vector<Mark>& M) {
  std::string path = std::string("test-data/") + name;  // the logs live in test-data/ at the repo root
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  char line[512];
  int64_t off = 0, last = -1;
  auto unwrap = [&](int64_t t) {  // 32-bit micros() rollover, file order = time order
    if (last >= 0 && t + off < last - (1LL << 31)) off += 1LL << 32;
    return last = t + off;
  };
  while (fgets(line, sizeof line, f)) {
    unsigned long us; long l, r, T;
    char lab[400];
    if (sscanf(line, "t=%lu us left : %ld raw right: %ld raw rawT= %ld", &us, &l, &r, &T) == 4)
      S.push_back({unwrap(us) / 1000, (float)l, (float)r});
    else if (sscanf(line, "############ MARK t=%lu us: %399[^#]", &us, lab) == 2) {
      std::string s(lab);
      while (!s.empty() && s.back() == ' ') s.pop_back();
      M.push_back({unwrap(us) / 1000, s});
    }
  }
  fclose(f);
  return true;
}

static std::string code(const kahvi::Reply& r) {
  switch (r.kind) {
    case kahvi::EMPTY: return "E";
    case kahvi::AWAY: return "A";
    case kahvi::BREWING: return "B";
    case kahvi::GONE: return "G";
    case kahvi::FAULT: return "F";
    case kahvi::MACHINE_AWAY: return "M";
    default: return std::to_string(r.cups);
  }
}

static bool ok(const std::string& want, const std::string& got) {
  if (want == "-") return true;
  size_t p = 0;
  while (true) {
    size_t q = want.find('|', p);
    if (want.substr(p, q == std::string::npos ? q : q - p) == got) return true;
    if (q == std::string::npos) return false;
    p = q + 1;
  }
}

// Synthetic failsafe scenarios (situations the calibration logs don't contain).
static int64_t T0;
static void hold(kahvi::Core& c, int secs, float L, float R) {  // steady platform, 1 sample/s
  for (int i = 0; i < secs; i++) { c.feed(T0, L, R); T0 += 1000; }
}
static int synthetic() {
  int fails = 0;
  auto check = [&](const char* what, bool good) { printf("  %s %s\n", good ? "ok  " : "FAIL", what); if (!good) fails++; };
  printf("== synthetic failsafes\n");
  // carafe pattern, dT > 0 = removed: dL = -0.3 * dT, dR = +1.3 * dT (A10: -26k / +115k for +87k)
  auto right = [](float dT, float& L, float& R) { L += -0.3f * dT; R += 1.3f * dT; };
  {  // 1. single-sample glitch
    kahvi::Core c; T0 = 1000; hold(c, 10, 0, 0);
    c.feed(T0, 3e6, -1e6); T0 += 1000; hold(c, 10, 0, 0);
    check("one wild sample changes nothing", code(c.reply(T0)) == "E");
  }
  {  // 2. whole machine taken away (machine pattern: dL/dT ~ +0.3) and brought back
    kahvi::Core c; T0 = 1000; c.potG = 500; hold(c, 10, 0, 0);
    hold(c, 10, 180000, 420000);
    bool away = code(c.reply(T0)) == "M";
    hold(c, 10, 0, 0);
    check("machine off -> 'Kahvinkeitin ei ole paikallaan', back -> fresh start (Tyhjä)", away && code(c.reply(T0)) == "E");
  }
  {  // 2b. someone leans hard on the machine (machine-sized, same pattern) and lets go
    kahvi::Core c; T0 = 1000; c.potG = 500; hold(c, 10, 0, 0);
    hold(c, 10, -180000, -420000); hold(c, 10, 0, 0);
    check("heavy lean + release is not 'machine away' (keeps 4 kuppia)", code(c.reply(T0)) == "4");
  }
  {  // 3. a missed carafe lift self-heals at the set-down
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    right(-(kahvi::EMPTY_CARAFE + 500 * kahvi::POT_PER_G), L, R);  // a carafe-sized set-down, "carafe already on"
    hold(c, 10, L, R);
    check("carafe-sized set-down while 'on' is read as a return (4 kuppia)", code(c.reply(T0)) == "4");
  }
  {  // 4. carafe away a long time
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    right(kahvi::EMPTY_CARAFE, L, R); hold(c, 60, L, R);
    bool a = code(c.reply(T0)) == "A";
    hold(c, 16 * 60, L, R);
    check("carafe away: A, after 15 min: 'Pannu ei ole paikallaan'", a && code(c.reply(T0)) == "G");
  }
  {  // 5. scale stops answering
    kahvi::Core c; T0 = 1000; hold(c, 10, 0, 0);
    check("no samples for 31 s -> FAULT", code(c.reply(T0 + 31000)) == "F");
    kahvi::Core fresh;
    check("no samples at all -> FAULT", code(fresh.reply(5000)) == "F");
  }
  {  // 6. flow-like movement for only 5 s (hand pushing the machine)
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 15, L, R);
    for (int i = 0; i < 5; i++) { L += 900; R -= 900; hold(c, 1, L, R); }
    hold(c, 30, L, R);
    check("5 s of flow-like movement is not a brew", code(c.reply(T0)) == "E");
  }
  {  // 7. flow that never stops (drift, stuck pattern) ends after 15 min
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 15, L, R);
    for (int i = 0; i < 20 * 60; i++) { L += 500; R -= 500; hold(c, 1, L, R); }
    hold(c, 5 * 60, L, R);
    check("endless flow stops being 'brewing' (15 min cap + tail)", code(c.reply(T0)) != "B");
  }
  {  // 8. an old fill that was never brewed doesn't inflate the next brew
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    c.fillG = 1250;  // stale: 10 cups "poured" earlier, but this brew is 2 cups
    for (int i = 0; i < 60; i++) { L += 214 * 4.2f; R -= 214 * 4.2f; hold(c, 1, L, R); }  // ~250 g moves
    hold(c, 5 * 60, L, R);
    check("stale 10-cup fill vs 2-cup brew -> 2 kuppia, not 10", code(c.reply(T0)) == "2");
  }
  {  // 9. power cut while the carafe was away; it was put back (with coffee) during the outage
    kahvi::Core c; T0 = 1000; float L = 0, R = 0;
    c.carafeOn = false; c.basketOn = true; c.potG = 0;  // what NVS restored
    hold(c, 10, L, R);
    right(kahvi::EMPTY_CARAFE + 600 * kahvi::POT_PER_G, L, R); hold(c, 10, L, R);   // someone lifts it
    bool away = code(c.reply(T0)) == "A";
    right(-(kahvi::EMPTY_CARAFE + 400 * kahvi::POT_PER_G), L, R); hold(c, 10, L, R); // pours, returns
    check("restored 'away' + real carafe lift/return self-heals (away, then 4 kuppia)", away && code(c.reply(T0)) == "4");
    right(38300, L, R); hold(c, 10, L, R);  // then the basket is emptied
    check("  ...and the basket afterwards is still the basket (4 kuppia)", code(c.reply(T0)) == "4");
  }
  {  // 10. grounds handled with the basket left in place (the most common workflow)
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; c.potG = 1000; hold(c, 10, L, R);  // fresh 10-cup pot
    right(51300, L, R); hold(c, 10, L, R);   // wet paper + grounds lifted out, basket stays
    bool first = code(c.reply(T0)) == "10";
    right(-19600, L, R); hold(c, 10, L, R);  // new paper + 70 g grounds put in place
    right(51300, L, R); hold(c, 10, L, R);   // ...and (after another brew) lifted out in place again
    check("in-place grounds removal twice is never read as the carafe (10 kuppia)", first && code(c.reply(T0)) == "10");
  }
  {  // 11. grounds added (heavy dose) while the carafe is at the sink
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    right(kahvi::EMPTY_CARAFE, L, R); hold(c, 10, L, R);  // carafe taken to be washed
    right(-22000, L, R); hold(c, 10, L, R);               // 80 g grounds + paper into the basket
    check("grounds added while the carafe is away don't count as the carafe returning", code(c.reply(T0)) == "A");
    right(-kahvi::EMPTY_CARAFE, L, R); hold(c, 10, L, R);
    check("  ...the carafe coming back does (Tyhjä)", code(c.reply(T0)) == "E");
  }
  {  // 12. water first, then grounds (basket lifted out for the grounds)
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    L -= 0.67f * 160000; R -= 0.33f * 160000; hold(c, 10, L, R);  // 7.5 dl into the reservoir
    right(38300, L, R); hold(c, 10, L, R); right(-38300 - 12400, L, R); hold(c, 10, L, R);  // basket out, back with 42 g
    check("water first, then grounds: fill kept (~750 g)", fabsf(c.fillG - 751) < 30);
  }
  {  // 13. reservoir filled to the brim (13.75 dl, run L) and brewed; carafe lifted with ~1150 g of coffee
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    L += 0.67f * -291000; R += 0.33f * -291000; hold(c, 20, L, R);  // 1375 g fill (-211.7/g measured)
    for (int i = 0; i < 330; i++) { L += 214.4f * 4.2f; R += -251.4f * 4.2f; hold(c, 1, L, R); }  // ~1375 g moves
    bool brewing = code(c.reply(T0)) == "B";
    hold(c, 300, L, R);
    bool full = code(c.reply(T0)) == "10";
    right(90000 + 1150 * kahvi::POT_PER_G, L, R); hold(c, 20, L, R);  // ~380k lift: must be the carafe, not the machine
    bool away = code(c.reply(T0)) == "A";
    right(-(90000 + 1150 * kahvi::POT_PER_G), L, R); hold(c, 20, L, R);
    check("brim-full brew: brewing, then 10 kuppia; its 380k carafe lift is the carafe (not the machine); back = 10",
          brewing && full && away && code(c.reply(T0)) == "10");
    right(90000 + 1250 * kahvi::POT_PER_G, L, R); hold(c, 20, L, R);  // extreme ~405k (> the 400k machine size)
    check("  ...even a 405k carafe lift (over the machine size) is read as the carafe", code(c.reply(T0)) == "A");
  }
  {  // 14. drip-stop: carafe taken the moment the reservoir empties (the basket holds the rest back)
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    L += 0.67f * -213000; R += 0.33f * -213000; hold(c, 20, L, R);  // 1000 g fill (8 cups)
    for (int i = 0; i < 240; i++) { L += 214.4f * 4.2f; R += -251.4f * 4.2f; hold(c, 1, L, R); }
    hold(c, 20, L, R);  // reservoir empty (flow ended), drip tail running: 840 g will reach the carafe
    right(90000 + 600 * kahvi::POT_PER_G, L, R); hold(c, 60, L, R);   // lifted with 600 g, 240 g held in the basket
    right(-(90000 + 350 * kahvi::POT_PER_G), L, R); hold(c, 300, L, R);  // 250 g poured, back; the basket drains in
    check("drip-stop, lifted at reservoir empty: 840 - 250 poured = 590 g -> 6 kuppia (old logic: 350 g -> 4)",
          code(c.reply(T0)) == "6");
  }
  {  // 15. drip-stop: carafe taken mid-brew, coffee poured, back while still brewing. Harsh version: the
     //     flow even pauses while the carafe is away (really it keeps filling the basket), so the bot first
     //     sees a false "reservoir empty" and then the flow resumes.
    kahvi::Core c; T0 = 1000; float L = 0, R = 0; hold(c, 10, L, R);
    L += 0.67f * -213000; R += 0.33f * -213000; hold(c, 20, L, R);
    for (int i = 0; i < 120; i++) { L += 214.4f * 4.2f; R += -251.4f * 4.2f; hold(c, 1, L, R); }  // half
    right(90000 + 380 * kahvi::POT_PER_G, L, R); hold(c, 40, L, R);   // lifted with 380 g
    right(-(90000 + 255 * kahvi::POT_PER_G), L, R); hold(c, 10, L, R);  // 125 g poured, back
    for (int i = 0; i < 120; i++) { L += 214.4f * 4.2f; R += -251.4f * 4.2f; hold(c, 1, L, R); }  // rest
    hold(c, 300, L, R);
    check("drip-stop, lifted mid-brew (even with a flow pause): 840 - 125 poured = 715 g -> 6 kuppia", code(c.reply(T0)) == "6");
  }
  {  // /kahvi spam guard
    kahvi::ReplyLimiter lim;
    int n = 0;
    for (int i = 0; i < 100; i++) n += lim.allow(-1001234567890LL, 1000 + i * 200);  // 100 in 20 s, one group
    check("spam: 100 /kahvi in 20 s from one group -> 2 answers (one per 10 s)", n == 2);
    check("  ...the same group 10 s later is answered again", lim.allow(-1001234567890LL, 1000 + 20000 + 9800));
    kahvi::ReplyLimiter g;
    n = 0;
    for (int i = 0; i < 40; i++) n += g.allow(1000 + i, 5000 + i * 100);  // 40 different chats in 4 s
    check("40 chats at once -> 12 answers (the per-minute cap)", n == 12);
    check("  ...a minute later answers resume", g.allow(7, 5000 + 60000) && g.allow(8, 5000 + 60100));
    int m = 0;
    for (int i = 0; i < 10; i++) m += g.allow(-1, 70000 + i * 10000);
    check("normal use, one /kahvi per 10 s, all answered", m == 10);
    kahvi::ReplyLimiter s;  // 20 /kahvi typed in ~6 s (Telegram's send times, 1 s resolution), handled slowly
    n = 0;
    for (int i = 0; i < 20; i++) n += s.allow(-42, (int64_t)(1790000000 + i * 3 / 10) * 1000);
    check("20 quick /kahvi (sent within 6 s) -> exactly 1 answer, however slowly they're handled", n == 1);
  }
  return fails;
}

int main() {
  // mark label prefix -> expected reply. Truth from the design notes result tables.
  struct Run { const char* file; std::vector<std::pair<const char*, const char*>> want; };
  std::vector<Run> runs = {
    {"calibration_log_20260923_1406.txt", {  // A: dry machine, covers, basket, carafe
      {"A0", "E"}, {"A1 r1", "E"}, {"A6 r1", "E"}, {"A7 r3", "E"}, {"A8", "E"}, {"A9", "E"},
      {"A10", "A"}, {"A11", "E"}, {"A12", "A"}, {"A13", "E"},
      {"A15", "-"},  // water poured straight into the carafe: not a real-use event
      {"A16", "A"}, {"A17", "E"}}},
    {"calibration_log_20260923_1452.txt", {  // B: 2 cups
      {"B0", "E"}, {"B2", "E"}, {"B4", "E"}, {"B5", "E"}, {"B7", "B|E"}, {"B8", "B"}, {"B9", "B|2"},
      {"B11", "2"}, {"B12", "2"}, {"B13", "A"}, {"B14", "2"}, {"B15 ", "A"}, {"B15a", "E"}, {"B16", "E"}}},
    {"calibration_log_20260923_1635.txt", {  // C: 10 cups, basket first
      {"C0", "E"}, {"C2", "E"}, {"C4", "E"}, {"C5", "E"}, {"C7", "B|E"}, {"C8", "B"}, {"C9", "B|10"},
      {"C10", "10"}, {"C11", "10"}, {"C12", "A"}, {"C13", "6"}, {"C14", "A"}, {"C15", "A|E"}, {"C16", "E"}}},
    {"calibration_log_20260923_1657.txt", {  // D: 10 cups, carafe first
      {"D0", "E"}, {"D2", "E"}, {"D5", "E"}, {"D7", "B|E"}, {"D8", "B"}, {"D9", "B|10"},
      {"D10", "A"}, {"D11", "A|8"}, {"D12", "8"}, {"D13", "8"}, {"D14", "A"}, {"D15", "A|E"}, {"D16", "E"}}},
    {"calibration_log_20260923_1842.txt", {  // E: 6 cups, carafe first
      {"E0", "E"}, {"E2", "E"}, {"E5", "E"}, {"E7", "B|E"}, {"E8", "B"}, {"E9", "B|6"},
      {"E10", "A"}, {"E11", "4"}, {"E12", "4"}, {"E13", "4"}, {"E14", "A"}, {"E15", "A|E"}, {"E16", "E"}}},
    {"calibration_log_20260923_1903.txt", {  // F: 6 cups water, 70 g grounds, basket first
      {"F0", "E"}, {"F2", "E"}, {"F5", "E"}, {"F7", "B|E"}, {"F8", "B"}, {"F9", "B|6"},
      {"F10", "6"}, {"F11", "6"}, {"F12", "A"}, {"F13", "4"}, {"F14", "A"}, {"F15", "A|E"}, {"F16", "E"}}},
    {"calibration_log_20260923_1927.txt", {  // G: 4 cups, basket left in
      {"G0", "E"}, {"G2", "E"}, {"G5", "E"}, {"G7", "B|E"}, {"G8", "B"}, {"G9", "B|4"},
      {"G10", "A"}, {"G11", "2"}, {"G12", "A"}, {"G13", "2"}, {"G14", "A"}, {"G15", "E"},
      {"G16", "E"}, {"G17", "E"}, {"G18", "E"}}},
    {"calibration_log_20260924_0657.txt", {  // H: 4 cups, basket first, never returned
      {"H0", "E"}, {"H2", "E"}, {"H5", "E"}, {"H7", "B|E"}, {"H8", "B"}, {"H9", "B|4"},
      {"H10", "4"}, {"H11", "A"}, {"H12", "2|E"}, {"H13", "A"}, {"H14", "E"}, {"H15", "E"}, {"H16", "E"}}},
    {"calibration_log_20260924_1321.txt", {  // J: 4 cups, grounds loaded in place, basket never lifted
      {"J0", "E"}, {"J1", "E"}, {"J2", "E"}, {"J3", "E"}, {"J5", "E"}, {"J6", "E"}, {"J8", "B|E"}, {"J9", "B"},
      {"J10", "B|4"}, {"J11", "4"}, {"J12", "4"}, {"J13", "4"},  // (a) grounds out/in in place
      {"J14", "A"}, {"J15", "2"}, {"J16", "2"}, {"J17", "2"},    // (b) coffee first, then grounds
      {"J18", "A"}, {"J19", "2"},                                // (c) carafe + grounds together
      {"J20", "A"}, {"J21", "A|E"}, {"J22 ", "E"}, {"J22.5", "E"}, {"J23", "E"}, {"J24", "E"}}},
    {"calibration_log_20260925_0737.txt", {  // K: whole machine taken off and put back; P: hands, lean, nudge
      {"K0", "E"}, {"K1", "A"}, {"K2", "M"}, {"K3", "E"}, {"K4", "E"}, {"K5", "M"}, {"K6", "E"},
      {"K7", "M"}, {"K8", "E"}, {"K9", "E"}, {"K10", "M"}, {"K11", "E"}, {"K12", "A"}, {"K13", "M"},
      {"K14", "E"},  // back with 2.5 dl in the carafe: fresh start assumes empty...
      {"K15", "A"}, {"K16", "2"},  // ...and the first lift re-measures it
      {"K17", "A"}, {"K18", "E"}, {"P1", "E"}, {"P2", "E"}, {"P3", "E"}, {"P4", "E"}, {"P5", "E"}, {"K19", "E"}}},
    {"calibration_log_20260925_1450.txt", {  // P: after rubber pads under the feet (no coffee)
      {"P0", "E"}, {"P1 ", "A"}, {"P2", "E"}, {"P3", "A"}, {"P4", "E"}, {"P5", "A"}, {"P6", "E"}, {"P7", "E"},
      {"P8 ", "E"}, {"P8b", "E"},
      {"P9 ", "-"}, {"P9b", "-"},  // water poured into the seated carafe: not a real-use event
      {"P10", "A"}, {"P11", "E"}, {"P12", "E"}, {"P13", "E"}, {"P14", "E"}, {"P15", "E"}}},
    {"calibration_log_20260925_1504.txt", {  // Q: drip-stop, carafe taken when the reservoir emptied (truth 276 g)
      {"Q0", "E"}, {"Q1", "E"}, {"Q2", "E"}, {"Q4", "B|E"}, {"Q5", "B"}, {"Q6", "B|A"},
      {"Q7", "2|B"}, {"Q8", "2"}, {"Q9", "A"}, {"Q10", "A|E"}, {"Q11", "E"}, {"Q12", "E"}}},
    {"calibration_log_20260925_1518.txt", {  // R: drip-stop, carafe taken mid-brew (truth 296 g; R7 mark is false)
      {"R0", "E"}, {"R1", "E"}, {"R2", "E"}, {"R4", "B|E"}, {"R5", "B|A"}, {"R6", "B"},
      {"R8", "2"}, {"R9", "A"}, {"R10", "A|E"}, {"R11", "E"}, {"R12", "E"}}},
  };

  int fails = 0, checks = 0;
  for (auto& run : runs) {
    std::vector<Sample> S;
    std::vector<Mark> M;
    if (!load(run.file, S, M)) { printf("cannot open %s (run from the repo root)\n", run.file); return 2; }
    // checkpoint: 6 s after the mark, but no later than halfway to the next mark
    std::vector<int64_t> cp(M.size());
    for (size_t i = 0; i < M.size(); i++) {
      int64_t c = M[i].t + 6000;
      if (i + 1 < M.size()) c = std::min(c, (M[i].t + M[i + 1].t) / 2);
      cp[i] = c;
    }
    kahvi::Core core;
    std::map<size_t, std::string> got;
    // Start at the first mark: log A begins before the calibration sketch's tare, and that jump
    // (-992k -> 0) looks exactly like the whole machine being lifted off. The bot never tares mid-run.
    size_t si = 0;
    while (si < S.size() && !M.empty() && S[si].t < M[0].t) si++;
    for (size_t i = 0; i < M.size(); i++) {
      while (si < S.size() && S[si].t <= cp[i]) { core.feed(S[si].t, S[si].L, S[si].R); si++; }
      got[i] = code(core.reply(cp[i]));
    }
    printf("== %s\n", run.file);
    for (auto& w : run.want) {
      bool found = false;
      for (size_t i = 0; i < M.size(); i++) {
        if (M[i].label.rfind(w.first, 0) != 0) continue;
        found = true;
        checks++;
        bool good = ok(w.second, got[i]);
        if (!good) fails++;
        printf("  %s %-45s want %-5s got %s\n", good ? "ok  " : "FAIL", M[i].label.c_str(), w.second, got[i].c_str());
        break;
      }
      if (!found) { printf("  FAIL mark '%s' not found\n", w.first); fails++; }
    }
  }
  fails += synthetic();
  char buf[160];
  kahvi::Reply r{kahvi::BREWING, 10, 3, 0};
  kahvi::format(r, nullptr, true, buf, sizeof buf);
  printf("\nsample text: %s\n", buf);
  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
