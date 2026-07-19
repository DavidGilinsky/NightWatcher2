// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/config.cpp
// Purpose:       Implementation of the INI-style configuration file parser.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace nightwatcher {

namespace {

std::string trim(const std::string& s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(s.begin(), s.end(), not_space);
    const auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return (begin < end) ? std::string(begin, end) : std::string();
}

}  // namespace

Config Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    Config cfg;
    std::string section;
    int current_sensor = -1;  // index into cfg.sensors, or -1

    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') {
            continue;  // blank line or comment
        }

        if (s.front() == '[' && s.back() == ']') {
            section = trim(s.substr(1, s.size() - 2));
            current_sensor = -1;
            static const std::string kSensorPrefix = "sensor:";
            if (section.rfind(kSensorPrefix, 0) == 0) {
                SensorConfig sc;
                sc.id = trim(section.substr(kSensorPrefix.size()));
                cfg.sensors.push_back(sc);
                current_sensor = static_cast<int>(cfg.sensors.size()) - 1;
            }
            continue;
        }

        const auto eq = s.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("malformed line " + std::to_string(lineno) +
                                     " in " + path + ": '" + s + "'");
        }
        const std::string key = trim(s.substr(0, eq));
        const std::string value = trim(s.substr(eq + 1));

        if (section == "daemon") {
            if (key == "interval") cfg.poll_interval_s = std::stoi(value);
        } else if (section == "database") {
            if (key == "host") cfg.db_host = value;
            else if (key == "name") cfg.db_name = value;
            else if (key == "user") cfg.db_user = value;
            else if (key == "port") cfg.db_port = std::stoi(value);
        } else if (section == "api") {
            if (key == "port") cfg.api_port = std::stoi(value);
            else if (key == "bind") cfg.api_bind = value;
            else if (key == "tls")
                cfg.api_tls = (value == "on" || value == "true" || value == "1" || value == "yes");
            else if (key == "tls_cert") cfg.api_tls_cert = value;
            else if (key == "tls_key") cfg.api_tls_key = value;
            else if (key == "schema_file") cfg.schema_file = value;
            else if (key == "web_root") cfg.web_root = value;
        } else if (current_sensor >= 0) {
            SensorConfig& sc = cfg.sensors[static_cast<size_t>(current_sensor)];
            if (key == "transport") sc.transport = value;
            else if (key == "address") sc.address = value;
            else if (key == "name") sc.name = value;
        }
        // Unknown sections/keys are ignored for forward compatibility.
    }

    // Default the TLS material to a tls/ dir beside the config file, so enabling
    // HTTPS from the UI has somewhere to auto-generate a self-signed cert.
    if (cfg.api_tls_cert.empty() || cfg.api_tls_key.empty()) {
        const auto slash = path.find_last_of('/');
        const std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
        if (cfg.api_tls_cert.empty()) cfg.api_tls_cert = dir + "/tls/nightwatcher-cert.pem";
        if (cfg.api_tls_key.empty()) cfg.api_tls_key = dir + "/tls/nightwatcher-key.pem";
    }

    return cfg;
}

}  // namespace nightwatcher
