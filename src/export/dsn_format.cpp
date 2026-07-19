// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/dsn_format.cpp
// Purpose:       Render SQM readings to the community "Light Pollution
//                Monitoring Data Format" (.dat) that the Dark Sky Network
//                ingests. Pure formatting (no network / no filesystem).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
//
// NOTE: The exact header line set, per-column precision/width, line endings and
// file-naming convention below are the community/UDM defaults and are pending
// reconciliation against a real DSN sample .dat + SN_ header. The row data
// (UTC/local time, temperature, counts, frequency, MSAS) comes straight from the
// `readings` table, so only presentation details should ever need to change.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "exporter.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace nightwatcher::exporter {
namespace {

// Line terminator for the data file (reconcile against the DSN sample).
constexpr const char* kEol = "\n";

std::mutex g_tz_mutex;

// Parse "YYYY-MM-DD HH:MM:SS" (UTC) into a time_t. Returns false if malformed.
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
    out = timegm(&tm);  // interpret the fields as UTC
    return true;
}

std::string fmt_iso(const std::tm& tm) {
    char b[80];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.000", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

// Local ISO time for a UTC time_t in the given IANA zone (empty -> UTC). Handles
// DST per-timestamp. Guarded because it temporarily sets the process TZ; safe
// here because the rest of the codebase timestamps with gmtime_r only.
std::string local_iso(time_t t, const std::string& tz) {
    std::tm tm{};
    if (tz.empty()) {
        gmtime_r(&t, &tm);
        return fmt_iso(tm);
    }
    std::lock_guard<std::mutex> lk(g_tz_mutex);
    const char* old = std::getenv("TZ");
    const bool had = old != nullptr;
    const std::string saved = had ? old : "";  // copy before setenv invalidates `old`
    setenv("TZ", tz.c_str(), 1);
    tzset();
    localtime_r(&t, &tm);
    if (had)
        setenv("TZ", saved.c_str(), 1);
    else
        unsetenv("TZ");
    tzset();
    return fmt_iso(tm);
}

// Resolve a header value: config override (non-empty string) else the fallback.
std::string cfg_or(const json& cfg, const char* key, const std::string& fallback) {
    if (cfg.contains(key) && cfg[key].is_string()) {
        const auto v = cfg[key].get<std::string>();
        if (!v.empty()) return v;
    }
    return fallback;
}

}  // namespace

std::string format_dsn_dat(const ExportContext& ctx, const std::string& config_json) {
    const json cfg = config_json.empty() ? json::object() : json::parse(config_json);
    const db::SensorRow& s = ctx.sensor;

    const std::string device_type = cfg_or(cfg, "device_type", s.model.empty() ? "SQM-LE" : s.model);
    const std::string instrument_id = cfg_or(cfg, "instrument_id", s.id);
    const std::string supplier = cfg_or(cfg, "data_supplier", s.name.empty() ? s.id : s.name);
    const std::string location = cfg_or(cfg, "location_name", s.site.empty() ? s.name : s.site);
    const std::string tz = cfg_or(cfg, "timezone", s.timezone);
    const std::string filter = cfg_or(cfg, "filter", "HOYA CM-500");
    const std::string time_sync = cfg_or(cfg, "time_sync", "NTP");
    const std::string fov = cfg_or(cfg, "fov", "20");
    const std::string cover_offset = cfg_or(cfg, "cover_offset", "0.00");
    const std::string firmware =
        s.feature_ver ? std::to_string(*s.feature_ver)
                      : (s.protocol_ver ? std::to_string(*s.protocol_ver) : std::string());

    char pos[96];
    std::snprintf(pos, sizeof pos, "%.6f, %.6f, %.1f", s.latitude.value_or(0.0),
                  s.longitude.value_or(0.0), s.elevation_m.value_or(0.0));

    std::vector<std::string> header = {
        "# Light Pollution Monitoring Data Format 1.0",
        "# URL: http://www.darksky.org/measurements",
        "# Number of header lines: {N}",
        "# This data is released under the following license: ODbL 1.0 "
        "http://opendatacommons.org/licenses/odbl/summary/",
        "# Device type: " + device_type,
        "# Instrument ID: " + instrument_id,
        "# Data supplier: " + supplier,
        "# Location name: " + location,
        "# Position (lat, lon, elev(m)): " + std::string(pos),
        "# Local timezone: " + tz,
        "# Time Synchronization: " + time_sync,
        "# Moving / Stationary position: STATIONARY",
        "# Moving / Fixed look direction: FIXED",
        "# Number of channels: 1",
        "# Filters per channel: " + filter,
        "# Measurement direction per channel: 0.000, 0.000",
        "# Field of view (degrees): " + fov,
        "# Number of fields per line: 6",
        "# SQM serial number: " + s.serial_number,
        "# SQM firmware version: " + firmware,
        "# SQM cover offset value: " + cover_offset,
        "# UTC Date & Time, Local Date & Time, Temperature, Counts, Frequency, MSAS",
        "# YYYY-MM-DDTHH:MM:SS.fff;YYYY-MM-DDTHH:MM:SS.fff;Celsius;counts;Hz;mag/arcsec^2",
        "# END OF HEADER",
    };
    // Self-referential header-line count (kept consistent regardless of which
    // optional lines are present).
    for (auto& h : header) {
        const auto p = h.find("{N}");
        if (p != std::string::npos) h.replace(p, 3, std::to_string(header.size()));
    }

    std::ostringstream out;
    for (const auto& h : header) out << h << kEol;

    for (const auto& r : ctx.readings) {
        time_t t = 0;
        std::string utc_iso, loc_iso;
        if (parse_sql_utc(r.ts_utc, t)) {
            std::tm gm{};
            gmtime_r(&t, &gm);
            utc_iso = fmt_iso(gm);
            loc_iso = local_iso(t, tz);
        } else {
            utc_iso = r.ts_utc;
            loc_iso = r.ts_utc;
        }
        char line[192];
        std::snprintf(line, sizeof line, "%s;%s;%.1f;%lld;%lld;%.2f", utc_iso.c_str(), loc_iso.c_str(),
                      r.temp_c, static_cast<long long>(r.period_counts),
                      static_cast<long long>(r.freq_hz), r.mag_arcsec2);
        out << line << kEol;
    }
    return out.str();
}

std::string dsn_file_name(const ExportContext& ctx, const std::string& config_json) {
    const json cfg = config_json.empty() ? json::object() : json::parse(config_json);
    std::string idpart = cfg_or(cfg, "site_id", ctx.sensor.id);

    // Date stamp = the day of the window's end (or the last reading).
    std::string when = ctx.to_ts;
    if (when.empty() && !ctx.readings.empty()) when = ctx.readings.back().ts_utc;
    if (when.empty()) when = ctx.from_ts;
    std::string ymd = "00000000";
    if (when.size() >= 10) ymd = when.substr(0, 4) + when.substr(5, 2) + when.substr(8, 2);
    return idpart + "_" + ymd + ".dat";
}

}  // namespace nightwatcher::exporter
