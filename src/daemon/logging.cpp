// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/logging.cpp
// Purpose:       Implementation of the daemon's stderr logging.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "logging.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace nightwatcher {
namespace {

std::mutex g_log_mutex;

const char* level_str(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

}  // namespace

void log_msg(LogLevel level, const std::string& msg) {
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
    gmtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fprintf(stderr, "%s [%s] %s\n", ts, level_str(level), msg.c_str());
    std::fflush(stderr);
}

}  // namespace nightwatcher
