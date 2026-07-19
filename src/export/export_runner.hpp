// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/export_runner.hpp
// Purpose:       One-shot export driver shared by the scheduler (nightly) and
//                the API (run-now): builds and uploads the DSN .dat for each
//                affected local month, logs it, and advances the watermark.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include "database.hpp"

namespace nightwatcher::exporter {

struct ExportRunResult {
    int files = 0;
    long long rows = 0;
    std::string last_ts;  // newest reading exported (the new watermark), or empty
    std::vector<std::string> file_names;
};

// Run one export for `target`: for every LOCAL month that has readings newer
// than the target's watermark, rebuild that month's .dat from all its readings,
// push it (create-or-update), record an export_log row, and finally advance the
// watermark. Returns a summary; throws (after logging an error row) on failure.
ExportRunResult run_export(db::Database& db, const db::ExportTargetRow& target);

}  // namespace nightwatcher::exporter
