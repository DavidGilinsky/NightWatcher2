// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/export_scheduler.cpp
// Purpose:       Implementation of the export scheduler: each worker sleeps
//                until its next run (nightly local time or fixed interval),
//                reloads its target, runs the export, and logs the outcome.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "export_scheduler.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "export_runner.hpp"
#include "logging.hpp"
#include "timeutil.hpp"

namespace tu = nightwatcher::exporter::timeutil;
using nightwatcher::log_error;
using nightwatcher::log_info;

namespace nightwatcher::exporter {
namespace {

// Parse "HH:MM" into hour/minute; defaults to 06:00 on anything malformed.
std::pair<int, int> parse_hhmm(const std::string& s) {
    int h = 6, m = 0;
    if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        h = 6;
        m = 0;
    }
    return {h, m};
}

}  // namespace

ExportScheduler::ExportScheduler(db::DbConfig db_cfg) : db_cfg_(std::move(db_cfg)) {}

ExportScheduler::~ExportScheduler() {
    stop();
    join();
}

void ExportScheduler::start(const std::vector<db::ExportTargetRow>& targets) {
    for (const auto& t : targets) {
        if (t.schedule == "manual") {
            log_info("export " + t.id + ": manual schedule (run on demand only)");
            continue;
        }
        threads_.emplace_back(&ExportScheduler::worker, this, t);
    }
}

void ExportScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
}

void ExportScheduler::join() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void ExportScheduler::worker(db::ExportTargetRow t) {
    std::unique_ptr<db::Database> db;
    try {
        db = std::make_unique<db::Database>(db_cfg_);
    } catch (const std::exception& e) {
        log_error("export " + t.id + ": cannot connect to database: " + e.what());
        return;
    }

    std::string tz;
    if (const auto s = db->find_sensor(t.sensor_id)) tz = s->timezone;

    log_info("export " + t.id + " -> " + t.target + " (" + t.schedule + ")");

    while (true) {
        // Reload the target each cycle so watermark/schedule/config edits and
        // deactivation take effect.
        const auto cur = db->find_export_target(t.id);
        if (!cur || cur->status != "active") {
            log_info("export " + t.id + ": no longer active; worker stopping");
            break;
        }

        int wait_s;
        if (cur->schedule == "interval") {
            wait_s = (cur->interval_s && *cur->interval_s > 0) ? *cur->interval_s : 3600;
        } else {  // nightly
            const auto [h, m] = parse_hhmm(cur->schedule_time);
            const time_t now = tu::utc_now();
            const time_t next = tu::next_local_hhmm_utc(h, m, tz, now);
            wait_s = static_cast<int>(next - now);
            if (wait_s < 1) wait_s = 1;
        }

        {
            std::unique_lock<std::mutex> lock(mtx_);
            if (cv_.wait_for(lock, std::chrono::seconds(wait_s), [this] { return stop_.load(); }))
                break;
        }

        try {
            const ExportRunResult res = run_export(*db, *cur);
            if (res.files > 0) {
                const std::string detail = std::to_string(res.rows) + " reading(s), " +
                                           std::to_string(res.files) + " file(s)";
                db->insert_event("export", "info", "export", detail, t.id);
                log_info("export " + t.id + ": " + detail);
            }
        } catch (const std::exception& e) {
            log_error("export " + t.id + ": " + e.what());
            try {
                db->insert_event("export", "error", "export_error", e.what(), t.id);
            } catch (const std::exception&) {
            }
        }
    }
}

}  // namespace nightwatcher::exporter
