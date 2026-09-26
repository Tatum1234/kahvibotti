// Long-run soak test: feeds kahvi::Core two simulated years of office use at 1 sample/s (~63 million
// samples) with sensor noise and slow drift, and checks after every event that /kahvi answers right.
// Also streams 30 years of brew records through the stats aggregation. Watches process memory:
// the core and stats code allocate nothing, so nothing may grow.
//   g++ -std=c++17 -O2 -o /tmp/soak firmware/kahvibotti/test/soak_test.cpp && /tmp/soak
#include "../kahvi_core.h"
#include "../stats_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <malloc.h>
#include <chrono>

static uint64_t rng = 12345;
static double rnd() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (rng >> 11) * (1.0 / 9007199254740992.0); }

struct Sim {
  kahvi::Core c;
  int64_t t = 1000;
  double L = 0, R = 0, drift = 0;
  long fails = 0, checks = 0;
  void tick(int secs) {  // quiet platform: noise +-200 counts, slow random-walk drift (bounded ~ +-8k)
    for (int i = 0; i < secs; i++) {
      drift += (rnd() - 0.5) * 20 - drift * 1e-5;
      c.feed(t, (float)(L + drift + (rnd() - .5) * 400), (float)(R + (rnd() - .5) * 400));
      t += 1000;
    }
  }
  void right(double dT) { L += -0.3 * dT; R += 1.3 * dT; }  // carafe/basket pattern (dT > 0 = removed)
  void reservoir(double dT) { L += 0.67 * dT; R += 0.33 * dT; }
  void expect(const char* what, const char* want) {
    kahvi::Reply r = c.reply(t);
    const char* got = r.kind == kahvi::EMPTY ? "E" : r.kind == kahvi::AWAY ? "A" : r.kind == kahvi::BREWING ? "B" :
                      r.kind == kahvi::MACHINE_AWAY ? "M" : r.kind == kahvi::GONE ? "G" : r.kind == kahvi::FAULT ? "F" : nullptr;
    char buf[8];
    if (!got) { snprintf(buf, sizeof buf, "%d", r.cups); got = buf; }
    checks++;
    bool ok = false;
    for (const char* w = want; *w;) {  // "4|E" style alternatives
      const char* e = w;
      while (*e && *e != '|') e++;
      if ((size_t)(e - w) == strlen(got) && !strncmp(w, got, e - w)) ok = true;
      w = *e ? e + 1 : e;
    }
    if (!ok && fails++ < 15) printf("  FAIL day %lld %s: want %s got %s (pot %.0f g)\n", (long long)(t / 86400000), what, want, got, c.potG);
  }
};

static int band(double cups) { return cups < 1 ? 0 : kahvi::band((float)cups); }

int main() {
  auto t0 = std::chrono::steady_clock::now();
  printf("soak: 2 simulated years...\n");  // stdio allocates its buffer on first use: before measuring
  size_t mem0 = mallinfo2().uordblks;
  Sim s;
  s.tick(30);
  const int DAYS = 730;
  long brews = 0;
  for (int day = 0; day < DAYS; day++) {
    int nBrews = 1 + (int)(rnd() * 3);
    for (int b = 0; b < nBrews; b++) {
      int cups = 2 * (1 + (int)(rnd() * 5));  // 2..10, filled to the printed lines
      double W = cups * 125.0;
      double dose = 7.0 * cups;                          // g of dry grounds, 7 g per cup
      s.right(-280 * dose); s.tick(20);                  // paper + grounds in, basket in place (4-20k, run J)
      s.reservoir(14000); s.tick(10);                    // reservoir lid off
      s.reservoir(-213 * W); s.tick(15);                 // fill
      s.reservoir(-14000); s.tick(20);                   // lid on
      double rate = 4.2 * (0.8 + rnd() * 0.4);           // g/s, varies with grind
      for (double moved = 0; moved < W; moved += rate) { s.L += 214.4 * rate; s.R += -251.4 * rate; s.tick(1); }
      s.expect("brewing", "B");
      s.tick(260);                                        // drip tail, the longest is ~231 s
      int want = band(W * 0.84 / 105);
      char w[8]; snprintf(w, sizeof w, "%d", want);
      s.expect("after brew", w);
      brews++;
      // people take coffee: lift, pour 1-3 cups, return, a few times
      double pot = W * 0.84;
      while (pot > 60) {
        s.tick(300 + (int)(rnd() * 3000));
        double carafe = 90000 + pot * 252;
        s.right(carafe); s.tick(20 + (int)(rnd() * 40));
        s.expect("carafe away", "A");
        pot = fmax(0, pot - 105 * (1 + (int)(rnd() * 3)));
        double seat = (rnd() - .5) * 20000;               // seating error, measured up to +-22k
        s.right(-(90000 + pot * 252) + seat); s.tick(20);
        int wb = band((pot - seat / 252) / 105);
        snprintf(w, sizeof w, "%d", wb);
        char alt[24]; snprintf(alt, sizeof alt, "%s|%d|%d|E", wb ? w : "E", wb + 2, wb > 2 ? wb - 2 : 0);
        s.expect("carafe back", alt);                    // the seating error may shift one band
      }
      s.right(90000 + pot * 252); s.tick(15); s.right(-90000); s.tick(20);  // emptied and rinsed
      s.expect("empty", "E");
      s.right(3000 + 694 * dose); s.tick(20);            // wet grounds lifted out in place (13-53k, runs B-J)
    }
    if (day % 30 == 29) {  // machine taken away and back once a month
      s.L += 190000; s.R += 450000; s.tick(600);
      s.expect("machine away", "M");
      s.L -= 190000; s.R -= 450000; s.tick(30);
      s.expect("machine back", "E");
    }
    s.tick((int)(86400 * 0.1));  // the rest of the day, in part (keeps the run short; drift continues)
  }
  size_t mem1 = mallinfo2().uordblks;
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf("== core: %d days, %ld brews, %lld samples, %ld checks, %ld failed, %.1f s, heap %zu -> %zu bytes\n",
         DAYS, brews, (long long)((s.t - 1000) / 1000), s.checks, s.fails, secs, mem0, mem1);
  int fails = s.fails ? 1 : 0;
  if (mem1 != mem0) { printf("  FAIL heap grew\n"); fails = 1; }

  // 30 years of brew records through the stats aggregation, streamed like the device does
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();
  stats::Totals tot;
  static float mc[32], yd[367], ym[13];
  static int mb[32];
  double sum = 0;
  long n = 0;
  uint32_t e = 1790000000u;  // 2026
  for (int d = 0; d < 365 * 30; d++) {
    for (int k = 0; k < 10; k++) {
      stats::Brew b = {e + (uint32_t)(d * 86400 + k * 3000), (uint16_t)(250 + 125 * (k % 9)), 0};
      stats::addTotals(b, 2040, 6, 15, tot);
      stats::addMonth(b, 2040, 6, mc, mb);
      stats::addYear(b, 2040, yd, ym);
      sum += stats::cups(b);
      n++;
    }
  }
  float yearSum = 0;
  for (int m = 1; m <= 12; m++) yearSum += ym[m];
  bool ok = fabs(tot.all - sum) < sum * 1e-3 && tot.brews == n && fabs(yearSum - tot.year) < 1 && tot.year > 0;
  printf("== stats: %ld records (30 years, %ld KB on flash), all-time %.0f cups (float sum drift %.4f %%), year 2040 %.0f: %s\n",
         n, n * 8 / 1024, tot.all, fabs(tot.all - sum) / sum * 100, tot.year, ok ? "ok" : "FAIL");
  if (!ok) fails = 1;
  printf("\n%s\n", fails ? "SOAK FAILED" : "soak ok");
  return fails;
}
