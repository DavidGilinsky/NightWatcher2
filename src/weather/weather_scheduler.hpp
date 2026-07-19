// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/weather/weather_scheduler.hpp
// Purpose:       Polling scheduler for weather stations: one worker thread per
//                station that fetches via its provider and stores observations.
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

namespace nightwatcher::weather {

class WeatherScheduler {
public:
    explicit WeatherScheduler(db::DbConfig db_cfg);
    ~WeatherScheduler();

    WeatherScheduler(const WeatherScheduler&) = delete;
    WeatherScheduler& operator=(const WeatherScheduler&) = delete;

    // Spawn one worker per station that has a provider (non-blocking).
    void start(const std::vector<db::WeatherStationRow>& stations);
    void stop();
    void join();

    int worker_count() const { return static_cast<int>(threads_.size()); }

private:
    void worker(db::WeatherStationRow station);

    db::DbConfig db_cfg_;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
};

}  // namespace nightwatcher::weather
