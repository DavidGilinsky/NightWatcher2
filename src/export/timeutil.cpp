// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/timeutil.cpp
// Purpose:       UTC/local time helpers (shared TZ mutex; DST-correct via the
//                system zoneinfo through localtime_r/mktime).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "timeutil.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace nightwatcher::exporter::timeutil {
namespace {

// Guards the transient setenv("TZ")/tzset() used by local_tm and tz_mktime.
// Shared by every timezone-sensitive helper here (and, via them, the DSN
// formatter) so the process TZ is never mutated by two threads at once.
std::mutex g_tz_mutex;

// Run `fn` with the process TZ set to `tz`, restoring it afterward. Caller holds
// g_tz_mutex.
template <typename Fn>
void with_tz(const std::string& tz, Fn&& fn) {
    const char* old = std::getenv("TZ");
    const bool had = old != nullptr;
    const std::string saved = had ? old : "";  // copy before setenv invalidates `old`
    setenv("TZ", tz.c_str(), 1);
    tzset();
    fn();
    if (had)
        setenv("TZ", saved.c_str(), 1);
    else
        unsetenv("TZ");
    tzset();
}

// Interpret a broken-down LOCAL time (in `tz`) as UTC time_t (inverse of local_tm).
time_t tz_mktime(std::tm tm, const std::string& tz) {
    tm.tm_isdst = -1;
    time_t out = 0;
    if (tz.empty()) return timegm(&tm);
    std::lock_guard<std::mutex> lk(g_tz_mutex);
    with_tz(tz, [&] { out = std::mktime(&tm); });
    return out;
}

}  // namespace

time_t utc_now() { return std::time(nullptr); }

bool parse_sql_utc(const std::string& s, time_t& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    out = timegm(&tm);
    return true;
}

std::string fmt_iso_ms(const std::tm& tm) {
    char b[80];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.000", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

std::string fmt_sql(const std::tm& tm) {
    char b[80];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

std::string fmt_sql_utc(time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    return fmt_sql(tm);
}

std::tm local_tm(time_t t, const std::string& tz) {
    std::tm tm{};
    if (tz.empty()) {
        gmtime_r(&t, &tm);
        return tm;
    }
    std::lock_guard<std::mutex> lk(g_tz_mutex);
    with_tz(tz, [&] { localtime_r(&t, &tm); });
    return tm;
}

std::string local_ym(const std::string& utc_sql, const std::string& tz) {
    time_t t = 0;
    if (!parse_sql_utc(utc_sql, t)) return utc_sql.substr(0, 7);
    const std::tm lt = local_tm(t, tz);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d", lt.tm_year + 1900, lt.tm_mon + 1);
    return b;
}

std::pair<std::string, std::string> local_month_bounds_utc(const std::string& ref_utc_sql,
                                                           const std::string& tz) {
    time_t t = 0;
    if (!parse_sql_utc(ref_utc_sql, t)) return {ref_utc_sql, ref_utc_sql};
    const std::tm lt = local_tm(t, tz);

    std::tm first{};
    first.tm_year = lt.tm_year;
    first.tm_mon = lt.tm_mon;
    first.tm_mday = 1;  // 00:00:00 local on the 1st
    const time_t start_utc = tz_mktime(first, tz);

    std::tm next = first;
    next.tm_mon += 1;  // mktime normalizes month overflow into the next year
    const time_t end_utc = tz_mktime(next, tz);

    return {fmt_sql_utc(start_utc), fmt_sql_utc(end_utc - 1)};
}

time_t next_local_hhmm_utc(int hour, int minute, const std::string& tz, time_t now) {
    std::tm target = local_tm(now, tz);
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;
    time_t cand = tz_mktime(target, tz);
    if (cand <= now) {
        target.tm_mday += 1;  // tomorrow (mktime normalizes)
        cand = tz_mktime(target, tz);
    }
    return cand;
}

time_t next_local_weekly_utc(int wday, int hour, int minute, const std::string& tz, time_t now) {
    if (wday < 0) wday = 0;
    if (wday > 6) wday = 6;
    std::tm target = local_tm(now, tz);
    const int delta = (wday - target.tm_wday + 7) % 7;  // days until the target weekday
    target.tm_mday += delta;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;
    time_t cand = tz_mktime(target, tz);
    if (cand <= now) {
        target.tm_mday += 7;  // next week (mktime normalizes)
        cand = tz_mktime(target, tz);
    }
    return cand;
}

time_t next_local_monthly_utc(int mday, int hour, int minute, const std::string& tz, time_t now) {
    if (mday < 1) mday = 1;
    if (mday > 28) mday = 28;  // clamp to a day that exists in every month
    std::tm target = local_tm(now, tz);
    target.tm_mday = mday;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;
    time_t cand = tz_mktime(target, tz);
    if (cand <= now) {
        target.tm_mon += 1;  // next month (mktime normalizes year)
        cand = tz_mktime(target, tz);
    }
    return cand;
}

}  // namespace nightwatcher::exporter::timeutil
