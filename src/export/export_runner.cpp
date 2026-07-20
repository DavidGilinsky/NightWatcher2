// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/export_runner.cpp
// Purpose:       Implementation of the one-shot export driver (affected-month
//                rebuild, logging, watermark advance).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "export_runner.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "exporter.hpp"
#include "nlohmann/json.hpp"
#include "timeutil.hpp"

using json = nlohmann::json;
namespace tu = nightwatcher::exporter::timeutil;

namespace nightwatcher::exporter {

namespace {
constexpr int kMaxRows = 200000;  // safety cap per query (~2 years at 5-min cadence)

// Weather station co-located with `sensor` (shared site, else the sole active
// station), and its readings across the reading window — for the webhook's
// ambient-temperature overlay. Empty when there is no clear co-located station.
std::vector<db::WeatherReadingRow> collect_ambient(db::Database& db,
                                                   const db::SensorRow& sensor,
                                                   const std::vector<db::ReadingRow>& readings) {
    if (readings.empty()) return {};
    std::string station_id;
    if (!sensor.site.empty()) {
        for (const auto& w : db.weather_stations())
            if (w.site == sensor.site) { station_id = w.id; break; }
    }
    if (station_id.empty()) {  // fall back to the sole active station, if exactly one
        const auto active = db.active_weather_stations();
        if (active.size() == 1) station_id = active.front().id;
    }
    if (station_id.empty()) return {};
    return db.weather_readings_between(station_id, readings.front().ts_utc,
                                       readings.back().ts_utc, kMaxRows);
}
}

ExportRunResult run_export(db::Database& db, const db::ExportTargetRow& t) {
    ExportRunResult summary;

    const auto sensor = db.find_sensor(t.sensor_id);
    if (!sensor)
        throw std::runtime_error("export target " + t.id + ": no such sensor '" + t.sensor_id + "'");

    // Timezone used for month bucketing (config override, else the sensor's).
    std::string tz = sensor->timezone;
    if (!t.config.empty()) {
        try {
            const json cfg = json::parse(t.config);
            if (cfg.contains("timezone") && cfg["timezone"].is_string())
                tz = cfg["timezone"].get<std::string>();
        } catch (const std::exception&) { /* config validated elsewhere */ }
    }

    const std::string now = tu::fmt_sql_utc(tu::utc_now());

    // Readings newer than the watermark (whole affected months are rebuilt, so a
    // boundary re-include would be harmless anyway).
    auto fresh = db.readings_between(t.sensor_id, t.last_export_ts, now, kMaxRows);
    if (!t.last_export_ts.empty()) {
        const std::string& wm = t.last_export_ts;
        fresh.erase(std::remove_if(fresh.begin(), fresh.end(),
                                   [&](const db::ReadingRow& r) { return r.ts_utc <= wm; }),
                    fresh.end());
    }
    if (fresh.empty()) return summary;  // nothing new

    std::string newest;
    for (const auto& r : fresh)
        if (r.ts_utc > newest) newest = r.ts_utc;

    auto exporter = make_exporter(t.target, t.config);

    // Incremental exporters (webhooks): push exactly the fresh readings, once;
    // the receiver de-dupes, so there is no whole-period rebuild.
    if (exporter->windowing() == Windowing::Incremental) {
        std::reverse(fresh.begin(), fresh.end());  // readings_between is DESC; send ascending

        ExportContext ctx;
        ctx.sensor = *sensor;
        ctx.readings = std::move(fresh);
        ctx.weather = collect_ambient(db, *sensor, ctx.readings);
        ctx.from_ts = t.last_export_ts;  // may be empty on the first run
        ctx.to_ts = now;

        db::ExportLogRow log;
        log.target_id = t.id;
        log.from_ts = ctx.from_ts;
        log.to_ts = now;
        try {
            const ExportResult res = exporter->run(ctx);
            log.row_count = res.row_count;
            log.file_name = res.file_name;
            log.remote_id = res.remote_id;
            log.status = res.row_count > 0 ? "ok" : "empty";
            db.insert_export_log(log);
            summary.files += 1;
            summary.rows += res.row_count;
            summary.file_names.push_back(res.file_name);
        } catch (const std::exception& e) {
            log.status = "error";
            log.detail = e.what();
            try { db.insert_export_log(log); } catch (const std::exception&) { /* best effort */ }
            throw;
        }

        db.set_export_watermark(t.id, newest);
        summary.last_ts = newest;
        return summary;
    }

    // Monthly exporters (DSN): rebuild each affected local-month in full.
    std::set<std::string> months;  // distinct local YYYY-MM
    for (const auto& r : fresh)
        months.insert(tu::local_ym(r.ts_utc, tz));

    for (const auto& ym : months) {
        std::string ref;
        for (const auto& r : fresh)
            if (tu::local_ym(r.ts_utc, tz) == ym) { ref = r.ts_utc; break; }
        const auto [from_utc, to_utc] = tu::local_month_bounds_utc(ref, tz);

        auto rows = db.readings_between(t.sensor_id, from_utc, to_utc, kMaxRows);
        std::reverse(rows.begin(), rows.end());  // readings_between is DESC; .dat wants ascending

        ExportContext ctx;
        ctx.sensor = *sensor;
        ctx.readings = std::move(rows);
        ctx.from_ts = from_utc;
        ctx.to_ts = to_utc;

        db::ExportLogRow log;
        log.target_id = t.id;
        log.from_ts = from_utc;
        log.to_ts = to_utc;
        try {
            const ExportResult res = exporter->run(ctx);
            log.row_count = res.row_count;
            log.file_name = res.file_name;
            log.remote_id = res.remote_id;
            log.status = res.row_count > 0 ? "ok" : "empty";
            db.insert_export_log(log);
            summary.files += 1;
            summary.rows += res.row_count;
            summary.file_names.push_back(res.file_name);
        } catch (const std::exception& e) {
            log.status = "error";
            log.detail = e.what();
            try { db.insert_export_log(log); } catch (const std::exception&) { /* best effort */ }
            throw;
        }
    }

    db.set_export_watermark(t.id, newest);
    summary.last_ts = newest;
    return summary;
}

}  // namespace nightwatcher::exporter
