#pragma once

#include <string>
#include <vector>

namespace nightwatcher {

// One [sensor:<id>] block from the configuration file.
struct SensorConfig {
    std::string id;         // e.g. "DSN003" (from the section name)
    std::string transport;  // "tcp" (SQM-LE) or "serial" (SQM-LU)
    std::string address;    // "host:port" (default port 10001) or "/dev/ttyUSB0"
    std::string name;       // human-readable label (optional)
};

// Parsed daemon configuration (simple INI-style file).
//
// Recognised sections:
//   [daemon]           interval = <seconds>
//   [database]         host / name / user
//   [api]              port
//   [sensor:<DSN-id>]  transport / address / name   (repeatable)
struct Config {
    int poll_interval_s = 300;                 // Dark Sky Network cadence default
    std::string db_host = "localhost";
    std::string db_name = "nightwatcher";
    std::string db_user = "nightwatcher";
    int api_port = 8080;
    std::vector<SensorConfig> sensors;

    // Load and parse the file at `path`.
    // Throws std::runtime_error on I/O failure or a malformed line.
    static Config load(const std::string& path);
};

}  // namespace nightwatcher
