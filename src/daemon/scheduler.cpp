// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/scheduler.cpp
// Purpose:       Implementation of the polling scheduler. Device errors trigger
//                reconnect with capped backoff; connect/disconnect transitions
//                and read errors are logged to the events table.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "device_factory.hpp"
#include "logging.hpp"
#include "sqm_device.hpp"

namespace nightwatcher {

Scheduler::Scheduler(db::DbConfig db_cfg) : db_cfg_(std::move(db_cfg)) {}

Scheduler::~Scheduler() {
    stop();
    join();
}

void Scheduler::start(const std::vector<db::SensorRow>& sensors) {
    for (const auto& s : sensors) {
        if (s.transport != "tcp" && s.transport != "serial") {
            log_warn("skipping sensor " + s.id + ": transport '" + s.transport +
                     "' not supported");
            continue;
        }
        threads_.emplace_back(&Scheduler::worker, this, s);
    }
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
}

void Scheduler::join() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void Scheduler::worker(db::SensorRow s) {
    std::unique_ptr<db::Database> db;
    try {
        db = std::make_unique<db::Database>(db_cfg_);
    } catch (const std::exception& e) {
        log_error("sensor " + s.id + ": cannot connect to database: " + e.what());
        return;
    }

    const int interval = s.poll_interval_s > 0 ? s.poll_interval_s : 300;

    bool up = false;        // last poll succeeded
    bool reported = false;  // we have logged the current state at least once
    int backoff = 2;        // seconds, doubles on repeated failure (capped)

    log_info("sensor " + s.id + " -> " + s.address + " every " + std::to_string(interval) + "s");

    while (true) {
        int wait_s = interval;

        sqm::Reading r;
        bool got = false;
        try {
            auto dev = sqm::open_device(s.transport, s.address, 5000);
            r = dev->read_averaged();
            got = true;
        } catch (const std::exception& e) {
            if (up || !reported) {
                try {
                    db->insert_event("sqm", "warning", "read_error", e.what(), s.id);
                } catch (const std::exception&) {
                }
                log_warn("sensor " + s.id + ": " + e.what());
            }
            up = false;
            reported = true;
            wait_s = std::min(interval, backoff);
            backoff = std::min(backoff * 2, 60);
        }

        if (got) {
            const std::string quality = (r.mag_arcsec2 <= 0.0) ? "saturated" : "ok";
            try {
                db->insert_reading(s.id, r, "", "poll", quality);
                if (!up) {
                    db->insert_event("sqm", "info", "connect", s.address, s.id);
                    up = true;
                }
                log_info("sensor " + s.id + ": mag=" + std::to_string(r.mag_arcsec2) +
                         " temp=" + std::to_string(r.temp_c) + " (" + quality + ")");
                reported = true;
                backoff = 2;
                wait_s = interval;
            } catch (const std::exception& e) {
                log_error("sensor " + s.id + ": database insert failed: " + e.what());
            }
        }

        std::unique_lock<std::mutex> lock(mtx_);
        if (cv_.wait_for(lock, std::chrono::seconds(wait_s), [this] { return stop_.load(); })) {
            break;  // stop requested
        }
    }

    log_info("sensor " + s.id + ": worker stopped");
}

}  // namespace nightwatcher
