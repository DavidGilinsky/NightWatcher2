// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/weather/weather_scheduler.cpp
// Purpose:       Implementation of the weather polling scheduler (fetch via
//                provider -> store; reconnect with backoff; event logging).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "weather_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "logging.hpp"
#include "provider.hpp"

namespace nightwatcher::weather {
namespace {
std::string opt_str(const std::optional<double>& v) {
    return v ? std::to_string(*v) : std::string("n/a");
}
}  // namespace

WeatherScheduler::WeatherScheduler(db::DbConfig db_cfg) : db_cfg_(std::move(db_cfg)) {}

WeatherScheduler::~WeatherScheduler() {
    stop();
    join();
}

void WeatherScheduler::start(const std::vector<db::WeatherStationRow>& stations) {
    for (const auto& s : stations) {
        if (s.provider.empty()) {
            log_warn("skipping weather station " + s.id + ": no provider configured");
            continue;
        }
        threads_.emplace_back(&WeatherScheduler::worker, this, s);
    }
}

void WeatherScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
}

void WeatherScheduler::join() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void WeatherScheduler::worker(db::WeatherStationRow s) {
    std::unique_ptr<db::Database> db;
    try {
        db = std::make_unique<db::Database>(db_cfg_);
    } catch (const std::exception& e) {
        log_error("weather " + s.id + ": cannot connect to database: " + e.what());
        return;
    }

    std::unique_ptr<IWeatherProvider> provider;
    try {
        provider = make_provider(s.provider, s.config);
    } catch (const std::exception& e) {
        log_error("weather " + s.id + ": " + e.what());
        try {
            db->insert_event("weather", "error", "config_error", e.what(), s.id);
        } catch (const std::exception&) {
        }
        return;
    }

    const int interval = s.poll_interval_s > 0 ? s.poll_interval_s : 300;
    bool up = false;
    bool reported = false;
    int backoff = 5;

    log_info("weather " + s.id + " -> " + s.provider + " every " + std::to_string(interval) + "s");

    while (true) {
        int wait_s = interval;

        db::WeatherReadingRow r;
        bool got = false;
        try {
            r = provider->fetch();
            r.station_id = s.id;
            got = true;
        } catch (const std::exception& e) {
            if (up || !reported) {
                try {
                    db->insert_event("weather", "warning", "fetch_error", e.what(), s.id);
                } catch (const std::exception&) {
                }
                log_warn("weather " + s.id + ": " + e.what());
            }
            up = false;
            reported = true;
            wait_s = std::min(interval, backoff);
            backoff = std::min(backoff * 2, 300);
        }

        if (got) {
            try {
                db->insert_weather_reading(r);
                if (!up) {
                    db->insert_event("weather", "info", "connect", s.provider, s.id);
                    up = true;
                }
                log_info("weather " + s.id + ": temp=" + opt_str(r.temp_c) +
                         " humidity=" + opt_str(r.humidity_pct));
                reported = true;
                backoff = 5;
                wait_s = interval;
            } catch (const std::exception& e) {
                log_error("weather " + s.id + ": database insert failed: " + e.what());
            }
        }

        std::unique_lock<std::mutex> lock(mtx_);
        if (cv_.wait_for(lock, std::chrono::seconds(wait_s), [this] { return stop_.load(); })) {
            break;
        }
    }

    log_info("weather " + s.id + ": worker stopped");
}

}  // namespace nightwatcher::weather
