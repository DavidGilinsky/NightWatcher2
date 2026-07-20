// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/exporter.hpp
// Purpose:       Pluggable data-export interface (mirrors the weather provider
//                pattern): each exporter formats a sensor's readings and pushes
//                them to an external network (Dark Sky Network, Globe at Night).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "database.hpp"  // db::SensorRow, db::ReadingRow

namespace nightwatcher::exporter {

// Everything an exporter needs for one run: the sensor's site metadata (for the
// file header) plus the readings to export (chronological, oldest first).
struct ExportContext {
    db::SensorRow sensor;
    std::vector<db::ReadingRow> readings;
    std::string from_ts;  // UTC window bounds, for file naming / logging
    std::string to_ts;
};

// Outcome of one export run.
struct ExportResult {
    std::string file_name;
    std::string remote_id;   // Drive file id / URL / local path
    long long row_count = 0;
};

// How run_export should window readings for an exporter:
//  - Monthly: rebuild each affected local-month in full (DSN — the destination
//    holds one idempotent file per month, so whole months are re-sent).
//  - Incremental: push only the readings newer than the watermark, once (webhooks
//    — the receiver de-dupes, so we send each reading just once).
enum class Windowing { Monthly, Incremental };

// A pluggable export target. run() formats the readings and pushes them to the
// endpoint; it throws std::runtime_error on any failure.
class IExporter {
public:
    virtual ~IExporter() = default;
    virtual ExportResult run(const ExportContext& ctx) = 0;
    virtual Windowing windowing() const { return Windowing::Monthly; }
};

// Build an exporter by target name from its JSON config. Throws on an unknown
// target, malformed config, or missing required settings.
//   "dsn"     : { site_id?, drive_folder_id?, outbox_dir?, oauth?{...}, header?{...} }
//   "webhook" : { url, token?, site_id?, batch?, insecure? }  (POST readings as JSON)
std::unique_ptr<IExporter> make_exporter(const std::string& target,
                                         const std::string& config_json);

// Render readings + site metadata to the DSN community data format (.dat).
// Pure (no network / no filesystem); exposed for unit testing. `config_json` may
// carry header overrides (device_type, instrument_id, data_supplier,
// location_name, filter, fov, cover_offset, time_sync, timezone).
std::string format_dsn_dat(const ExportContext& ctx, const std::string& config_json);

// The file name a DSN export would use for the given context/config.
std::string dsn_file_name(const ExportContext& ctx, const std::string& config_json);

}  // namespace nightwatcher::exporter
