// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/export_scheduler.hpp
// Purpose:       Scheduler for data-export targets: one worker thread per
//                target that runs on a nightly (local HH:MM) or interval cadence.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "database.hpp"

namespace nightwatcher::exporter {

class ExportScheduler {
public:
    explicit ExportScheduler(db::DbConfig db_cfg);
    ~ExportScheduler();

    ExportScheduler(const ExportScheduler&) = delete;
    ExportScheduler& operator=(const ExportScheduler&) = delete;

    // Spawn one worker per scheduled (nightly/interval) target (non-blocking).
    void start(const std::vector<db::ExportTargetRow>& targets);
    void stop();
    void join();

    int worker_count() const { return static_cast<int>(threads_.size()); }

private:
    void worker(db::ExportTargetRow target);

    db::DbConfig db_cfg_;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
};

}  // namespace nightwatcher::exporter
