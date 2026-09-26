// Brew statistics: records, calendar helpers, totals and milestones. Plain C++ (no Arduino), so
// test/stats_test.cpp runs it on a PC. Storage (a LittleFS file of Brew records) lives in the sketch.
//
// Local time: set TZ to "EET-2EEST,M3.5.0/3,M10.5.0/4" (Finland) before using the date helpers.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>

namespace stats {

// One finished brew, appended to the stats file (8 bytes: ~30 years at 10 brews/day in 896 KB).
struct Brew {
  uint32_t epoch;   // when the brew finished (UTC seconds)
  uint16_t waterG;  // water brewed, grams
  uint16_t pad;
};

constexpr float CUP_WATER_G = 125;  // 1 kuppi = 1.25 dl on the reservoir scale, as in the bot's replies

inline float cups(const Brew& b) { return b.waterG / CUP_WATER_G; }

// ---------- milestones (Telegram "easter eggs") ----------

struct Milestone { int cups; const char* text; };
const Milestone MILESTONES[] = {
  {100, "☕ Ensimmäiset 100 kuppia keitetty!"},
  {5000, "🏆 5000 kuppia keitetty! Se on noin 625 litraa kahvia."},
  {10000, "🏆🏆 10 000 KUPPIA! Legendaarinen saavutus."},
};
constexpr int MILESTONE_COUNT = sizeof(MILESTONES) / sizeof(MILESTONES[0]);

// The lowest milestone reached but not yet announced (announced = cups of the last one sent,
// persisted), or nullptr. Call again after sending: several can be due after a long offline spell.
inline const Milestone* due(float totalCups, int announced) {
  for (const Milestone& m : MILESTONES)
    if (m.cups > announced && totalCups >= m.cups) return &m;
  return nullptr;
}

// The next goal (for the admin page), or nullptr when all are reached.
inline const Milestone* nextGoal(float totalCups) {
  for (const Milestone& m : MILESTONES)
    if (totalCups < m.cups) return &m;
  return nullptr;
}

// ---------- calendar ----------

inline bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline int daysInMonth(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : d[m - 1];
}

inline int weekday(int y, int m, int d) {  // Monday = 0 ... Sunday = 6 (Sakamoto)
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int sun0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;  // Sunday = 0
  return (sun0 + 6) % 7;
}

inline int dayOfYear(int y, int m, int d) {  // 1-based
  int n = d;
  for (int i = 1; i < m; i++) n += daysInMonth(y, i);
  return n;
}

inline int isoWeeksInYear(int y) {
  auto p = [](int y) { return (y + y / 4 - y / 100 + y / 400) % 7; };
  return (p(y) == 4 || p(y - 1) == 3) ? 53 : 52;
}

inline int isoWeek(int y, int m, int d) {  // ISO 8601 week number ("viikko")
  int w = (dayOfYear(y, m, d) - (weekday(y, m, d) + 1) + 10) / 7;
  if (w < 1) return isoWeeksInYear(y - 1);
  if (w > isoWeeksInYear(y)) return 1;
  return w;
}

inline void localDate(uint32_t epoch, int& y, int& m, int& d) {
  time_t t = epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  y = tm.tm_year + 1900; m = tm.tm_mon + 1; d = tm.tm_mday;
}

// ---------- aggregation ----------
// The stats file is streamed one record at a time into these accumulators (the whole history
// never has to fit in RAM). The array versions below are for tests.

inline int isoWeekYear(int y, int m, int d) {  // the year an ISO week belongs to (1.1.2027 -> 2026)
  int w = isoWeek(y, m, d);
  if (w == 1 && m == 12) return y + 1;
  if (w >= 52 && m == 1) return y - 1;
  return y;
}

// cups[1..31] and brews[1..31] of one month (zero them first).
inline void addMonth(const Brew& b, int y, int m, float cupsOut[32], int brewsOut[32]) {
  int yy, mm, dd;
  localDate(b.epoch, yy, mm, dd);
  if (yy == y && mm == m) { cupsOut[dd] += cups(b); brewsOut[dd]++; }
}

// cups per day of the year (index = day of year 1..366) and per month [1..12] (zero them first).
inline void addYear(const Brew& b, int y, float dayOut[367], float monthOut[13]) {
  int yy, mm, dd;
  localDate(b.epoch, yy, mm, dd);
  if (yy != y) return;
  dayOut[dayOfYear(yy, mm, dd)] += cups(b);
  monthOut[mm] += cups(b);
}

struct Totals { float today = 0, week = 0, month = 0, year = 0, acad = 0, all = 0; int brews = 0; };

// The university's academic year (lukuvuosi) starts on 1 August: returns its first calendar year.
inline int acadYear(int y, int m) { return m >= 8 ? y : y - 1; }

// Totals for "now" (local date y-m-d): today, this ISO week, this month, this year, this academic
// year, all time.
inline void addTotals(const Brew& b, int y, int m, int d, Totals& t) {
  int yy, mm, dd;
  localDate(b.epoch, yy, mm, dd);
  float c = cups(b);
  t.all += c;
  t.brews++;
  if (yy == y) t.year += c;
  if (acadYear(yy, mm) == acadYear(y, m)) t.acad += c;
  if (yy == y && mm == m) t.month += c;
  if (yy == y && mm == m && dd == d) t.today += c;
  if (isoWeek(yy, mm, dd) == isoWeek(y, m, d) && isoWeekYear(yy, mm, dd) == isoWeekYear(y, m, d)) t.week += c;
}

inline void monthDays(const Brew* b, size_t n, int y, int m, float cupsOut[32], int brewsOut[32]) {
  for (int i = 0; i < 32; i++) { cupsOut[i] = 0; brewsOut[i] = 0; }
  for (size_t i = 0; i < n; i++) addMonth(b[i], y, m, cupsOut, brewsOut);
}

inline void yearDays(const Brew* b, size_t n, int y, float dayOut[367], float monthOut[13]) {
  for (int i = 0; i < 367; i++) dayOut[i] = 0;
  for (int i = 0; i < 13; i++) monthOut[i] = 0;
  for (size_t i = 0; i < n; i++) addYear(b[i], y, dayOut, monthOut);
}

inline float totalCups(const Brew* b, size_t n) {
  float s = 0;
  for (size_t i = 0; i < n; i++) s += cups(b[i]);
  return s;
}

// ---------- calendar layout ----------

// Month calendar rows: rows[r][0] = ISO week number, rows[r][1..7] = day of month Mon..Sun, 0 = blank.
// Returns the number of rows (4-6).
inline int monthGrid(int y, int m, int rows[6][8]) {
  int first = weekday(y, m, 1), days = daysInMonth(y, m), r = 0;
  for (int i = 0; i < 6; i++) for (int j = 0; j < 8; j++) rows[i][j] = 0;
  for (int d = 1; d <= days; d++) {
    int col = (first + d - 1) % 7;
    r = (first + d - 1) / 7;
    rows[r][col + 1] = d;
    if (!rows[r][0]) rows[r][0] = isoWeek(y, m, d);
  }
  return r + 1;
}

// Year heatmap position of a date: column = week of the year's grid (starting from the week of
// 1 January), row = weekday Mon..Sun.
inline void yearCell(int y, int m, int d, int& col, int& row) {
  col = (dayOfYear(y, m, d) - 1 + weekday(y, 1, 1)) / 7;
  row = weekday(y, m, d);
}

inline const char* monthName(int m) {
  static const char* n[] = {"", "Tammikuu", "Helmikuu", "Maaliskuu", "Huhtikuu", "Toukokuu", "Kesäkuu",
                            "Heinäkuu", "Elokuu", "Syyskuu", "Lokakuu", "Marraskuu", "Joulukuu"};
  return m >= 1 && m <= 12 ? n[m] : "";
}

// UTC epoch of an HTTP Date header value, e.g. "Fri, 25 Sep 2026 17:30:00 GMT" (any letter case), or -1.
// The clock source of last resort: Telegram's replies carry it when NTP (UDP 123) is blocked.
inline int64_t httpDate(const char* s) {
  const char* c = s;
  while (*c && *c != ',') c++;
  if (*c != ',') return -1;
  int d, y, h, mi, se;
  char mon[4] = "";
  if (sscanf(c + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6) return -1;
  static const char* M = "janfebmaraprmayjunjulaugsepoctnovdec";
  int m = 0;
  for (int i = 0; i < 12 && !m; i++) {
    bool same = true;
    for (int k = 0; k < 3; k++) same = same && (mon[k] | 0x20) == M[i * 3 + k];
    if (same) m = i + 1;
  }
  if (!m || d < 1 || d > 31 || y < 2020 || y > 2200 || h > 23 || mi > 59 || se > 60) return -1;
  int yy = m <= 2 ? y - 1 : y;  // days from civil (H. Hinnant), valid for any Gregorian date
  int era = yy / 400, yoe = yy - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + se;
}

}  // namespace stats
