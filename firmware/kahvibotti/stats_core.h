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
// The organisation's own list (2026-09-26), sorted by cups (due() relies on it).
// At ~5 000 cups a year the last ones will never come: they're jokes. Stored per device is only the cups
// of the last one sent (NVS "stats"/"ms"), so changing this list needs no flash clean-up.
const Milestone MILESTONES[] = {
  {10, "Ensimmäiset 10 kuppia keitetty! Keittämiseen kului noin 150 Wh sähköä, eli saman verran kuin 11 puhelimen lataukseen."},
  {100, "100 kuppia keitetty! Hervannan ratikka ajaisi keittämiseen kuluneella sähköllä noin 230 metriä."},
  {1200, "1200 kuppia keitetty! Se on täyden kylpyammeen verran vettä."},
  {4000, "4000 kuppia keitetty! Keittämiseen on mennyt vettä noin maalämpökaivon keruuliuoksen verran."},
  {6000, "6000 kuppia keitetty! Keittämiseen kulunut sähkö (noin 90 kWh) riittäisi ratikalle koko linjan 3 matkaan Hervantajärveltä Sorin aukiolle."},
  {8500, "8500 kuppia keitetty! Kahvipuruja on kulunut noin 60kg eli yhden kahvisäkin verran."},
  {10000, "10 000 kuppia keitetty! Keittämiseen on kulunut 1250 litraa vettä, sillä täyttäisi teekkarisaunan paljun."},
  {13600, "13 600 kuppia keitetty! Vettä on keitetty 2500 kVA:n jakelumuuntajan öljyn verran."},
  {15000, "15 000 kuppia keitetty! Sähköä on kulunut noin 230kWh. Olkiluoto 3 tuottaa saman noin puolessa sekunnissa."},
  {20000, "20 000 kuppia keitetty! Keittämiseen kuluneella sähköllä (noin 300 kWh) saisi elektrolyysillä noin 6 kg vetyä. Vetyautolla sillä ajaisi yli 600 km."},
  {25000, "25 000 kuppia keitetty! Keittämiseen on kulunut noin 380 kWh sähköä, vähän enemmän kuin 400 watin aurinkopaneeli tuottaa Suomessa vuodessa."},
  {30000, "30 000 kuppia keitetty! Keittämiseen on kulunut noin 460 kWh sähköä, eli noin 50 saunakerran verran."},
  {35000, "35 000 kuppia keitetty! Keittämiseen on kulunut noin 530 kWh sähköä. Koko Suomi kuluttaa saman määrän sähköä noin viidesosasekunnissa."},
  {40000, "40 000 kuppia keitetty! Vettä on kulunut 5 000 litraa, eli 25 täyttä kylpyammetta."},
  {45000, "45 000 kuppia keitetty! Kahvipuruja on kulunut noin 315 kg, eli yli viiden kahvisäkin verran."},
  {50000, "50 000 kuppia keitetty! Keittämiseen on kulunut noin 760kWh sähköä. Yksi Tesla Megapack akkukontti (3,9 MWh) riittäisi noin viisi kertaa näin monen kupin keittämiseen."},
  {64000, "64 000 kuppia keitetty! 110 kV sähköaseman päämuuntajan öljyn verran."},
  {100000, "100 000 kuppia keitetty! En usko että tämä on mahdollista!"},
  {240000, "240 000 kuppia keitetty! Keittämiseen on kulunut kahvia maitorekan säiliön verran (30 000 l)."},
  {560000, "560 000 kuppia keitetty! Juoksuttaessa Tammerkoskesta menee sama määrän vettä sekunnissa."},
  {1000000, "Miljoona kuppia!! Lopettakaa jo se kahvin juonti."},
  {8000000, "8 Mijoonaa kuppia!! Hervannan vesitorniin menee tmän verran."},
  {10000000, " 10 Miljoonaa kuppia! Pliis heittäkää tämä jo roskiin."},
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

// ---------- the Tilastot page, fast ----------
// The page used to read the whole history and convert every record's time to a local date three times
// (~25 us each on the C3): 3 s per view after 10 years of brews, 7.6 s with the storage full. Records are
// appended as brews finish, so the file is in time order: the page now jumps (binary search) to the part it
// shows and converts each record once. Constant time however many years are stored. The plain add*
// functions above stay as the reference; test/stats_test.cpp checks that both give the same numbers.

// "Now" for the totals, worked out once per page instead of once per record.
struct Today {
  int y, m, d, week, weekYear, acad;
  Today(int y_, int m_, int d_)
      : y(y_), m(m_), d(d_), week(isoWeek(y_, m_, d_)), weekYear(isoWeekYear(y_, m_, d_)), acad(acadYear(y_, m_)) {}
};

// What one view shows. Only the kind that was asked for is complete: `month` for a month view, `yearDay`/
// `yearMonth` for a year view (the other holds just what the read range happened to contain). `t.all`/
// `t.brews` are not filled: the device keeps them as running sums.
struct Page {
  Totals t;
  float month[32] = {};                          // cups per day of the viewed month (1..31)
  float yearDay[367] = {}, yearMonth[13] = {};   // the viewed year: per day of year (1..366), per month
};

// One record into the page, with a single local-date conversion. Totals only when `totals`.
inline void addPage(const Brew& b, const Today& n, int y, int m, bool totals, Page& p) {
  int yy, mm, dd;
  localDate(b.epoch, yy, mm, dd);
  float c = cups(b);
  if (totals) {
    if (yy == n.y) p.t.year += c;
    if (acadYear(yy, mm) == n.acad) p.t.acad += c;
    if (yy == n.y && mm == n.m) p.t.month += c;
    if (yy == n.y && mm == n.m && dd == n.d) p.t.today += c;
    if (isoWeek(yy, mm, dd) == n.week && isoWeekYear(yy, mm, dd) == n.weekYear) p.t.week += c;
  }
  if (yy == y) {
    if (mm == m) p.month[dd] += c;
    p.yearDay[dayOfYear(yy, mm, dd)] += c;
    p.yearMonth[mm] += c;
  }
}

// Local midnight of y-m-d as epoch seconds (the TZ must be set). 64-bit: page years go past 2106.
inline int64_t localMidnight(int y, int m, int d) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = d;
  t.tm_isdst = -1;  // let mktime decide (DST)
  return (int64_t)mktime(&t);
}

constexpr int64_t PAGE_MARGIN = 86400;  // read a day extra at each edge: DST and clock-step slack

// Index of the first record with epoch >= e in a time-ordered file of `count` records; at(i) = epoch of i.
template <typename At> inline size_t firstAtOrAfter(size_t count, int64_t e, At at) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if ((int64_t)at(mid) < e) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Fills p for one view (month y-m, or year y when yearView). count = records in the file; at(i) = epoch of
// record i; each(from, to, fn) calls fn(record) for records [from, to). If the file isn't in time order
// (`ordered` false: a big clock step once), every record is read, still with one conversion each.
// Pass 1: from the earliest date the totals need (1 January or the academic year's 1 August, whichever is
// earlier) to the end: totals + whatever of the viewed period is in it. Pass 2, only when browsing an older
// period: just that period, up to where pass 1 began (split by index, so nothing is counted twice).
template <typename At, typename Each>
inline void fillPage(size_t count, bool ordered, At at, Each each, const Today& n, int y, int m, bool yearView,
                     Page& p) {
  p = Page();
  auto both = [&](const Brew& b) { addPage(b, n, y, m, true, p); };
  if (!ordered) { each(0, count, both); return; }
  int64_t totalsFrom = localMidnight(n.y, 1, 1);
  int64_t acadFrom = localMidnight(n.acad, 8, 1);
  if (acadFrom < totalsFrom) totalsFrom = acadFrom;
  size_t cur = firstAtOrAfter(count, totalsFrom - PAGE_MARGIN, at);
  each(cur, count, both);
  int64_t viewFrom = yearView ? localMidnight(y, 1, 1) : localMidnight(y, m, 1);
  size_t v0 = firstAtOrAfter(cur, viewFrom - PAGE_MARGIN, at);  // only records before pass 1 matter here
  if (v0 < cur) {
    int64_t viewTo = yearView ? localMidnight(y + 1, 1, 1) : localMidnight(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1);
    size_t v1 = firstAtOrAfter(cur, viewTo + PAGE_MARGIN, at);
    each(v0, v1, [&](const Brew& b) { addPage(b, n, y, m, false, p); });
  }
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
