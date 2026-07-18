// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/logging.hpp
// Purpose:       Minimal thread-safe, timestamped logging to stderr (captured by
//                journald when running under systemd).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace nightwatcher {

enum class LogLevel { Info, Warn, Error };

void log_msg(LogLevel level, const std::string& msg);

inline void log_info(const std::string& m) { log_msg(LogLevel::Info, m); }
inline void log_warn(const std::string& m) { log_msg(LogLevel::Warn, m); }
inline void log_error(const std::string& m) { log_msg(LogLevel::Error, m); }

}  // namespace nightwatcher
