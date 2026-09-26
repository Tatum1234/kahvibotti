// Checks stats_core.h: calendar maths (ISO weeks, weekdays), Finnish local dates across DST and
// midnight, month/year aggregation and milestones. Reference values from Python's datetime/zoneinfo.
//   g++ -std=c++17 -O2 -o /tmp/stats_test firmware/kahvibotti/test/stats_test.cpp && /tmp/stats_test
#include "../stats_core.h"
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

  printf("== HTTP Date (clock fallback)\n");
  check("\"Fri, 25 Sep 2026 17:30:00 GMT\" = 1790357400", httpDate("Fri, 25 Sep 2026 17:30:00 GMT") == 1790357400);
  check("lower case (headers are lowercased) + leap day 29 Feb 2028", httpDate("tue, 29 feb 2028 00:00:00 gmt") == 1835395200);
  check("year 2100 (not a leap year): 1 Mar 2100", httpDate("Mon, 01 Mar 2100 00:00:00 GMT") == 4107542400LL);
  check("garbage and missing parts give -1", httpDate("nonsense") == -1 && httpDate("Fri, 25 Sep 2026") == -1 &&
        httpDate("Fri, 25 Xyz 2026 17:30:00 GMT") == -1);
  printf("== milestones\n");
  check("99 cups: nothing due", due(99.9f, 0) == nullptr);
  const Milestone* ms = due(100.2f, 0);
  check("100 cups: the first one is due", ms && ms->cups == 100);
  check("...and once sent, it isn't due again", due(150, 100) == nullptr);
  check("next goal after 150 cups is 5000", nextGoal(150) && nextGoal(150)->cups == 5000);
  ms = due(10050, 100);
  check("offline past 5000 and 10000: 5000 comes first", ms && ms->cups == 5000);
  check("...then 10000", due(10050, 5000) && due(10050, 5000)->cups == 10000);
  check("all reached: no next goal", nextGoal(12000) == nullptr && due(12000, 10000) == nullptr);
  check("the 5000 text is the approved one", strcmp(MILESTONES[1].text, "🏆 5000 kuppia keitetty! Se on noin 625 litraa kahvia.") == 0);

  printf("\n%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
  return fails ? 1 : 0;
}
