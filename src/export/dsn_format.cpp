// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/dsn_format.cpp
// Purpose:       Render SQM readings to the "Community Standard Skyglow Data
//                Format 1.0" (.dat) that the Dark Sky Network ingests. Pure
//                formatting (no network / no filesystem).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
//
// Reconciled byte-for-byte against a real DSN site file
// (sample/DSN036-S_Gilinsky_24-12.dat, produced by SQM Reader Pro):
//   * 35-line "# ..." header, CRLF terminators.
//   * Position: "+lat.4, +lon.4, +elev(int)"; serial with leading zeros stripped.
//   * Data rows: UTC;Local;Temp(.1f);Counts;Frequency;MSAS  (6 fields). For
//     SQM-LE/LU the Counts and Frequency fields are left EMPTY by convention
//     (set "include_counts_freq": true in config to populate them). MSAS is
//     printed with trailing zeros stripped (e.g. 15.70 -> "15.7").
//   * File name: "<site_id>[_<supplier>]_<YY-MM>.dat" using the LOCAL month.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include "exporter.hpp"
#include "nlohmann/json.hpp"
#include "timeutil.hpp"

using json = nlohmann::json;
namespace tu = nightwatcher::exporter::timeutil;

namespace nightwatcher::exporter {
namespace {

// DSN files use CRLF terminators.
constexpr const char* kEol = "\r\n";

// Resolve a header value: config override (non-empty string) else the fallback.
std::string cfg_or(const json& cfg, const char* key, const std::string& fallback) {
    if (cfg.contains(key) && cfg[key].is_string()) {
        const auto v = cfg[key].get<std::string>();
        if (!v.empty()) return v;
    }
    return fallback;
}

// Strip trailing zeros (and a trailing dot) from a decimal string: 15.70 -> 15.7.
std::string trim_zeros(std::string s) {
    if (s.find('.') == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Unihedron serials are stored zero-padded ("00007141"); the DSN header uses the
// integer form ("7141"). Non-numeric serials pass through unchanged.
std::string serial_int(const std::string& s) {
    if (s.empty()) return s;
    for (char c : s)
        if (c < '0' || c > '9') return s;
    return std::to_string(std::atoll(s.c_str()));
}

std::string fmt_msas(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return trim_zeros(b);
}

}  // namespace

std::string format_dsn_dat(const ExportContext& ctx, const std::string& config_json) {
    const json cfg = config_json.empty() ? json::object() : json::parse(config_json);
    const db::SensorRow& s = ctx.sensor;
    const std::string tz = cfg_or(cfg, "timezone", s.timezone);
    const bool include_cf = cfg.value("include_counts_freq", false);

    char pos[96];
    std::snprintf(pos, sizeof pos, "%+.4f, %+.4f, %+d", s.latitude.value_or(0.0),
                  s.longitude.value_or(0.0), static_cast<int>(std::lround(s.elevation_m.value_or(0.0))));

    // Exactly 35 header lines, matching the Community Standard Skyglow Data
    // Format. Fields we do not have default to blank (the DSN accepts blanks);
    // filler comment/blank lines pad the header to its fixed length.
    const std::vector<std::string> header = {
        "# Community Standard Skyglow Data Format 1.0",
        "# URL: http://www.darksky.org/NSBM/sdf1.0.pdf",
        "# Number of header lines: 35",
        "# This data is released under the following license: ODbL 1.0 "
        "http://opendatacommons.org/licenses/odbl/summary/",
        "# Device type: " + cfg_or(cfg, "device_type", s.model.empty() ? "SQM-LE/LU" : s.model),
        "# Instrument ID: " + cfg_or(cfg, "instrument_id", s.id),
        "# Data supplier: " + cfg_or(cfg, "data_supplier", ""),
        "# Location name: " + cfg_or(cfg, "location_name", s.site.empty() ? s.name : s.site),
        "# Position (lat, lon, elev(m)): " + std::string(pos),
        "# Local timezone: " + tz,
        "# Time Synchronization: " + cfg_or(cfg, "time_sync", "NTP"),
        "# Moving / Stationary position: STATIONARY",
        "# Moving / Fixed look direction: FIXED",
        "# Number of channels: 1",
        "# Filters per channel: " + cfg_or(cfg, "filter", "HOYA CM-500"),
        "# Measurement direction per channel: 0, 0",
        "# Field of view (degrees): " + cfg_or(cfg, "fov", "20"),
        "# Number of fields per line: 6",
        "# SQM serial number: " + serial_int(s.serial_number),
        "# SQM firmware version: " + cfg_or(cfg, "firmware", ""),
        "# SQM cover offset value: " + cfg_or(cfg, "cover_offset", "0"),
        "# SQM readout test ix: " + cfg_or(cfg, "ix_test", ""),
        "# SQM readout test rx: " + cfg_or(cfg, "rx_test", ""),
        "# SQM readout test cx: " + cfg_or(cfg, "cx_test", ""),
        "# Comment: " + cfg_or(cfg, "comment", "Data acquired by NightWatcher2"),
        "# Comment: ",
        "# Comment: ",
        "# Comment: ",
        "# Comment: ",
        "# blank line 30",
        "# blank line 31",
        "# blank line 32",
        "# UTC Date & Time, Local Date & Time, Temperature, Counts, Frequency, MSAS",
        "# YYYY-MM-DDTHH:mm:ss.fff;YYYY-MM-DDTHH:mm:ss.fff;Celsius;number;Hz;mag/arcsec^2",
        "# END OF HEADER",
    };

    std::ostringstream out;
    for (const auto& h : header) out << h << kEol;

    for (const auto& r : ctx.readings) {
        time_t t = 0;
        std::string utc_iso, loc_iso;
        if (tu::parse_sql_utc(r.ts_utc, t)) {
            std::tm gm{};
            gmtime_r(&t, &gm);
            utc_iso = tu::fmt_iso_ms(gm);
            loc_iso = tu::fmt_iso_ms(tu::local_tm(t, tz));
        } else {
            utc_iso = r.ts_utc;
            loc_iso = r.ts_utc;
        }
        std::string counts, freq;
        if (include_cf) {
            counts = std::to_string(static_cast<long long>(r.period_counts));
            freq = std::to_string(static_cast<long long>(r.freq_hz));
        }
        char temp[16];
        std::snprintf(temp, sizeof temp, "%.1f", r.temp_c);
        out << utc_iso << ';' << loc_iso << ';' << temp << ';' << counts << ';' << freq << ';'
            << fmt_msas(r.mag_arcsec2) << kEol;
    }
    return out.str();
}

std::string dsn_file_name(const ExportContext& ctx, const std::string& config_json) {
    const json cfg = config_json.empty() ? json::object() : json::parse(config_json);
    const std::string site_id = cfg_or(cfg, "site_id", ctx.sensor.id);
    const std::string supplier = cfg_or(cfg, "supplier", "");
    const std::string tz = cfg_or(cfg, "timezone", ctx.sensor.timezone);

    // The file is bucketed by the LOCAL year-month of the window's end.
    std::string when = ctx.to_ts;
    if (when.empty() && !ctx.readings.empty()) when = ctx.readings.back().ts_utc;
    if (when.empty()) when = ctx.from_ts;

    const std::string ym = tu::local_ym(when, tz);          // "YYYY-MM"
    const std::string yymm = ym.size() >= 7 ? ym.substr(2) : "00-00";  // "YY-MM"

    std::string name = site_id;
    if (!supplier.empty()) name += "_" + supplier;
    name += "_" + yymm + ".dat";
    return name;
}

}  // namespace nightwatcher::exporter
