// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/timeutil.hpp
// Purpose:       UTC/local time helpers for the exporter (timezone conversion
//                and local-calendar-month bounds). All timezone-sensitive calls
//                share one mutex, since they set the process TZ transiently.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <ctime>
#include <string>
#include <utility>

namespace nightwatcher::exporter::timeutil {

time_t utc_now();

// Parse "YYYY-MM-DD HH:MM:SS" (UTC) into a time_t. Returns false if malformed.
bool parse_sql_utc(const std::string& s, time_t& out);

std::string fmt_iso_ms(const std::tm& tm);  // "YYYY-MM-DDTHH:MM:SS.000"
std::string fmt_sql(const std::tm& tm);     // "YYYY-MM-DD HH:MM:SS"
std::string fmt_sql_utc(time_t t);          // gmtime(t) as "YYYY-MM-DD HH:MM:SS"

// Broken-down LOCAL time for a UTC time_t in the given IANA zone (empty -> UTC).
std::tm local_tm(time_t t, const std::string& tz);

// Local "YYYY-MM" of a UTC "YYYY-MM-DD HH:MM:SS" timestamp.
std::string local_ym(const std::string& utc_sql, const std::string& tz);

// UTC [start, end] SQL strings (both inclusive) covering the LOCAL calendar
// month that contains the reference UTC timestamp. `end` is the last second of
// the month, so it pairs with a `ts <= end` query.
std::pair<std::string, std::string> local_month_bounds_utc(const std::string& ref_utc_sql,
                                                           const std::string& tz);

// UTC time_t of the next occurrence of local time hour:minute (today if still
// ahead of `now`, otherwise tomorrow).
time_t next_local_hhmm_utc(int hour, int minute, const std::string& tz, time_t now);

// UTC time_t of the next occurrence of local weekday `wday` (0=Sun..6=Sat) at
// hour:minute (this week if still ahead of `now`, otherwise next week).
time_t next_local_weekly_utc(int wday, int hour, int minute, const std::string& tz, time_t now);

// UTC time_t of the next occurrence of local day-of-month `mday` at hour:minute
// (this month if still ahead of `now`, else next month). `mday` is clamped to
// 1..28; `mday <= 0` is a sentinel meaning "the last day of the month".
time_t next_local_monthly_utc(int mday, int hour, int minute, const std::string& tz, time_t now);

}  // namespace nightwatcher::exporter::timeutil
