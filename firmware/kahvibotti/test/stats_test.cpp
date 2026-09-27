// Checks stats_core.h: calendar maths (ISO weeks, weekdays), Finnish local dates across DST and
// midnight, month/year aggregation and milestones. Reference values from Python's datetime/zoneinfo.
//   g++ -std=c++17 -O2 -o /tmp/stats_test firmware/kahvibotti/test/stats_test.cpp && /tmp/stats_test
#include "../stats_core.h"
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

static int fails = 0;
static void check(const char* what, bool good) {
  printf("  %s %s\n", good ? "ok  " : "FAIL", what);
  if (!good) fails++;
}

int main() {
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);  // the same string the ESP32 uses
  tzset();
  using namespace stats;

  printf("== calendar\n");
  check("2026-09-25 is a Friday, week 39", weekday(2026, 9, 25) == 4 && isoWeek(2026, 9, 25) == 39);
  check("2027-01-01 belongs to 2026 week 53", isoWeek(2027, 1, 1) == 53);
  check("2026-12-31 week 53", isoWeek(2026, 12, 31) == 53);
  check("2024-12-30 is week 1 (of 2025)", isoWeek(2024, 12, 30) == 1);
  check("2021-01-03 is 2020 week 53, a Sunday", isoWeek(2021, 1, 3) == 53 && weekday(2021, 1, 3) == 6);
  check("2026-01-01 week 1, Thursday", isoWeek(2026, 1, 1) == 1 && weekday(2026, 1, 1) == 3);
  check("Feb 2028 has 29 days, Feb 2026 has 28", daysInMonth(2028, 2) == 29 && daysInMonth(2026, 2) == 28);

  printf("== local dates (Europe/Helsinki)\n");
  int y, m, d;
  localDate(1790368200, y, m, d);  // 2026-09-25 23:30 local = 20:30 UTC
  check("23:30 local counts on the same day (25.9.)", y == 2026 && m == 9 && d == 25);
  localDate(1790285400, y, m, d);  // 2026-09-25 00:30 local = 24.9. 21:30 UTC
  check("00:30 local counts on the new day, not the UTC one (25.9.)", d == 25);
  localDate(1774747800, y, m, d);  // 2026-03-29 03:30, just after the spring DST switch
  check("right after spring DST switch (29.3.)", m == 3 && d == 29);
  localDate(1792888200, y, m, d);  // 2026-10-25 03:30, the autumn switch hour
  check("autumn DST switch (25.10.)", m == 10 && d == 25);

  printf("== aggregation\n");
  Brew b[] = {
    {1790368200, 1250, 0},  // 25.9. 23:30, 10 cups
    {1790285400, 500, 0},   // 25.9. 00:30, 4 cups
    {1774747800, 750, 0},   // 29.3., 6 cups
    {1798797600, 250, 0},   // 1.1.2027, 2 cups
  };
  size_t n = sizeof(b) / sizeof(b[0]);
  float c[32]; int br[32];
  monthDays(b, n, 2026, 9, c, br);
  check("September 2026: 25th = 14 cups from 2 brews", fabsf(c[25] - 14) < 0.01f && br[25] == 2 && c[24] == 0);
  float day[367], mon[13];
  yearDays(b, n, 2026, day, mon);
  check("2026 by month: Sep 14, Mar 6, Jan 0 (the 2027 brew excluded)", fabsf(mon[9] - 14) < 0.01f && fabsf(mon[3] - 6) < 0.01f && mon[1] == 0);
  check("2026 heatmap: day-of-year 268 (25.9.) = 14", fabsf(day[dayOfYear(2026, 9, 25)] - 14) < 0.01f && dayOfYear(2026, 9, 25) == 268);
  check("total 22 cups", fabsf(totalCups(b, n) - 22) < 0.01f);

  printf("== calendar layout + period totals\n");
  int rows[6][8];
  int nr = monthGrid(2026, 9, rows);
  check("Sep 2026: 5 rows, starts Tue 1st in week 36", nr == 5 && rows[0][0] == 36 && rows[0][1] == 0 && rows[0][2] == 1);
  check("Sep 2026: last row week 40 = 28, 29, 30", rows[4][0] == 40 && rows[4][1] == 28 && rows[4][3] == 30 && rows[4][4] == 0);
  nr = monthGrid(2026, 2, rows);
  check("Feb 2026 (starts Sunday): 5 rows, 1st alone on Sunday of week 5", nr == 5 && rows[0][7] == 1 && rows[0][6] == 0 && rows[0][0] == 5);
  nr = monthGrid(2027, 1, rows);
  check("Jan 2027: first row is 2026 week 53", rows[0][0] == 53 && rows[0][5] == 1);
  check("1.1.2027 is ISO year 2026, 30.12.2024 is ISO year 2025", isoWeekYear(2027, 1, 1) == 2026 && isoWeekYear(2024, 12, 30) == 2025);
  int col, row;
  yearCell(2026, 1, 1, col, row);
  check("year grid: 1.1.2026 (Thu) in column 0, row 3", col == 0 && row == 3);
  yearCell(2026, 1, 5, col, row);
  check("year grid: 5.1.2026 (Mon) starts column 1", col == 1 && row == 0);
  Totals t;
  for (size_t i = 0; i < n; i++) addTotals(b[i], 2026, 9, 25, t);
  check("totals on 25.9.2026: today 14, week 14, month 14, year 20, all 22, 4 brews",
        fabsf(t.today - 14) < .01f && fabsf(t.week - 14) < .01f && fabsf(t.month - 14) < .01f &&
        fabsf(t.year - 20) < .01f && fabsf(t.all - 22) < .01f && t.brews == 4);
  check("lukuvuosi 2026-2027 on 25.9.2026: the 25.9. and 1.1.2027 brews, not 29.3.2026", fabsf(t.acad - 16) < .01f);
  check("lukuvuosi starts 1.8.: 31.7.2027 is still 2026, 1.8.2027 is 2027",
        acadYear(2027, 7) == 2026 && acadYear(2027, 8) == 2027);
  Totals t2;
  for (size_t i = 0; i < n; i++) addTotals(b[i], 2026, 12, 31, t2);
  check("totals on 31.12.2026: the 1.1.2027 brew is in the same ISO week (53)", fabsf(t2.week - 2) < .01f && fabsf(t2.year - 20) < .01f);
  check("month names", strcmp(monthName(9), "Syyskuu") == 0 && strcmp(monthName(12), "Joulukuu") == 0);

  printf("== Tilastot fast path (fillPage) vs the reference (addTotals/addMonth/addYear over everything)\n");
  {
    // 12 years of brews, ~11 a day at varying times, in time order (as the device appends them).
    std::vector<Brew> f;
    uint32_t e = 1790357400u - 12u * 365 * 86400;
    for (int i = 0; e < 1790357400u; i++) {
      f.push_back({e, (uint16_t)(250 + 125 * (i % 9)), 0});
      e += 3000 + (i * 7919) % 9000;  // 50 min - 3 h 20 min apart
    }
    auto at = [&](size_t i) { return f[i].epoch; };
    size_t read = 0;
    auto each = [&](size_t from, size_t to, auto fn) { for (size_t i = from; i < to; i++) { read++; fn(f[i]); } };
    struct { int ty, tm, td, y, m; bool yv; } cases[] = {
      {2026, 9, 25, 2026, 9, false}, {2026, 9, 25, 2026, 9, true},    // today's views
      {2026, 9, 25, 2019, 3, false}, {2026, 9, 25, 2020, 1, true},    // browsing old periods (pass 2)
      {2026, 9, 25, 2025, 11, false}, {2026, 9, 25, 2025, 1, true},   // periods straddling where pass 1 starts
      {2026, 3, 29, 2026, 3, false},                                   // the DST switch day
      {2026, 1, 1, 2026, 1, false}, {2026, 1, 2, 2025, 12, false},     // ISO week 1 starting in December
      {2021, 1, 3, 2021, 1, false},                                    // ISO week 53 of 2020 in January 2021
      {2026, 7, 31, 2026, 7, false}, {2026, 8, 1, 2026, 8, false},     // the academic year's last and first day
      {2014, 10, 5, 2014, 10, false}, {2030, 6, 1, 2030, 6, true},     // the first weeks; long after the last
    };
    bool allSame = true, allFast = true;
    for (auto& c : cases) {
      // The file as it was at the end of that "today" (the device only ever has records up to now).
      size_t n = firstAtOrAfter(f.size(), localMidnight(c.ty, c.tm, c.td + 1), at);
      Totals rt;
      static float rmc[32], ryd[367], rym[13];
      static int rmb[32];
      monthDays(f.data(), n, c.y, c.m, rmc, rmb);
      yearDays(f.data(), n, c.y, ryd, rym);
      for (size_t i = 0; i < n; i++) addTotals(f[i], c.ty, c.tm, c.td, rt);
      for (bool ordered : {true, false}) {
        static Page p;
        read = 0;
        fillPage(n, ordered, at, each, Today(c.ty, c.tm, c.td), c.y, c.m, c.yv, p);
        auto eq = [](float a, float b) { return fabsf(a - b) < .01f; };
        bool same = eq(p.t.today, rt.today) && eq(p.t.week, rt.week) && eq(p.t.month, rt.month) &&
                    eq(p.t.year, rt.year) && eq(p.t.acad, rt.acad);
        if (c.yv) for (int i = 0; i < 367; i++) same = same && eq(p.yearDay[i], ryd[i]) && (i > 12 || eq(p.yearMonth[i], rym[i]));
        else for (int i = 0; i < 32; i++) same = same && eq(p.month[i], rmc[i]);
        allSame = allSame && same;
        // ~11 a day: a year and a half at most, never the whole history
        if (ordered && read > 12000) { allFast = false; printf("  read %zu for %d.%d.%d\n", read, c.td, c.tm, c.ty); }
        if (!same) printf("  DIFF: today %d.%d.%d, view %d-%d%s, %s\n", c.td, c.tm, c.ty, c.y, c.m, c.yv ? " (year)" : "",
                          ordered ? "ordered" : "unordered");
      }
    }
    check("14 views (DST day, ISO weeks 1/53, academic year edges, old periods): same numbers as the reference, "
          "ordered and unordered", allSame);
    check("ordered file: reads at most ~1.5 years of the 12 (not the whole history)", allFast);
    // A clock step back of a few hours (NTP correcting a Telegram-set clock) leaves records slightly out of
    // order: the day of margin must still catch them.
    std::vector<Brew> g = f;
    for (size_t i = 1000; i + 1 < g.size(); i += 997) std::swap(g[i].epoch, g[i + 1].epoch);
    f.swap(g);
    Totals rt;
    for (auto& b : f) addTotals(b, 2026, 9, 25, rt);
    static Page p;
    fillPage(f.size(), true, at, each, Today(2026, 9, 25), 2026, 9, false, p);
    check("records a few hours out of order: still the same totals", fabsf(p.t.year - rt.year) < .01f &&
          fabsf(p.t.acad - rt.acad) < .01f && fabsf(p.t.week - rt.week) < .01f);
    // Right at the edge where the page starts reading (1.1.2026 for "today" 25.9.2026): a New Year's Day brew
    // stored before three New Year's Eve brews. The binary search must still start early enough to see it.
    f.swap(g);  // back to the ordered file
    size_t k = firstAtOrAfter(f.size(), localMidnight(2026, 1, 1), at);
    std::rotate(f.begin() + (k - 3), f.begin() + k, f.begin() + k + 1);
    Totals re;
    for (auto& b : f) addTotals(b, 2026, 9, 25, re);
    fillPage(f.size(), true, at, each, Today(2026, 9, 25), 2026, 9, false, p);
    check("a brew stored out of order exactly at the start of the year is still counted (the day of margin)",
          fabsf(p.t.year - re.year) < .01f);
    fillPage(0, true, at, each, Today(2026, 9, 25), 2026, 9, true, p);
    check("empty file: all zero", p.t.year == 0 && p.t.acad == 0 && p.yearMonth[9] == 0);
  }
  printf("== HTTP Date (clock fallback)\n");
  check("\"Fri, 25 Sep 2026 17:30:00 GMT\" = 1790357400", httpDate("Fri, 25 Sep 2026 17:30:00 GMT") == 1790357400);
  check("lower case (headers are lowercased) + leap day 29 Feb 2028", httpDate("tue, 29 feb 2028 00:00:00 gmt") == 1835395200);
  check("year 2100 (not a leap year): 1 Mar 2100", httpDate("Mon, 01 Mar 2100 00:00:00 GMT") == 4107542400LL);
  check("garbage and missing parts give -1", httpDate("nonsense") == -1 && httpDate("Fri, 25 Sep 2026") == -1 &&
        httpDate("Fri, 25 Xyz 2026 17:30:00 GMT") == -1);
  printf("== milestones\n");
  check("9.9 cups: nothing due", due(9.9f, 0) == nullptr);
  const Milestone* ms = due(10.2f, 0);
  check("10 cups: the first one is due", ms && ms->cups == 10);
  check("...and once sent, it isn't due again", due(50, 10) == nullptr);
  check("next goal after 50 cups is 100", nextGoal(50) && nextGoal(50)->cups == 100);
  ms = due(6100, 100);
  check("offline past 1200, 4000 and 6000: 1200 comes first", ms && ms->cups == 1200);
  check("...then 4000", due(6100, 1200) && due(6100, 1200)->cups == 4000);
  check("all reached: no next goal", nextGoal(1e8f) == nullptr && due(1e8f, 10000000) == nullptr);
  bool sorted = true, texts = true;
  for (int i = 0; i < MILESTONE_COUNT; i++) {
    if (i && MILESTONES[i].cups <= MILESTONES[i - 1].cups) sorted = false;  // due() takes the first match
    size_t n = strlen(MILESTONES[i].text);
    if (n < 10 || n > 4096) texts = false;  // Telegram's message limit is 4096 characters
  }
  check("milestones sorted by cups, texts non-empty and within Telegram's limit", sorted && texts);
  check("23 milestones, from 10 to 10 000 000 cups", MILESTONE_COUNT == 23 && MILESTONES[0].cups == 10 &&
        MILESTONES[MILESTONE_COUNT - 1].cups == 10000000);
  check("the 10-cup text is the approved one", strcmp(MILESTONES[0].text, "Ensimmäiset 10 kuppia keitetty! "
        "Keittämiseen kului noin 150 Wh sähköä, eli saman verran kuin 11 puhelimen lataukseen.") == 0);

  printf("\n%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
  return fails ? 1 : 0;
}
