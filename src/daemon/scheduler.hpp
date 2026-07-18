// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/scheduler.hpp
// Purpose:       Polling scheduler: one worker thread per SQM sensor that reads
//                on the sensor's interval, stores readings, and logs events.
// Created:       2026-07-18
// Last Modified: 2026-07-18
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

namespace nightwatcher {

// Owns the polling worker threads. Each worker uses its own database connection
// (Database is not thread-safe). Not copyable.
class Scheduler {
public:
    explicit Scheduler(db::DbConfig db_cfg);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Spawn one worker per tcp sensor (non-tcp sensors are skipped with a warning).
    // Non-blocking.
    void start(const std::vector<db::SensorRow>& sensors);
    // Ask all workers to stop at their next wake-up (non-blocking).
    void stop();
    // Join all worker threads.
    void join();

    int worker_count() const { return static_cast<int>(threads_.size()); }

private:
    void worker(db::SensorRow sensor);

    db::DbConfig db_cfg_;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
};

}  // namespace nightwatcher
