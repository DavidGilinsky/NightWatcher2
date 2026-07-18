// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/daemon/main.cpp
// Purpose:       Entry point for nightwatcherd: argument parsing, configuration
//                loading, and the startup summary (poll/serve loop lands later).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// ---------------------------------------------------------------------------
#include <iostream>
#include <string>

#include "config.hpp"
#include "nightwatcher/version.hpp"

namespace {

void print_version() {
    std::cout << "nightwatcherd " << NIGHTWATCHER_VERSION << "\n";
}

void print_help(const char* argv0) {
    std::cout
        << "nightwatcherd " << NIGHTWATCHER_VERSION
        << " \xe2\x80\x94 NightWatcher2 SQM daemon\n\n"
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  -c, --config <path>   Path to the INI-style config file\n"
        << "  -v, --version         Print version and exit\n"
        << "  -h, --help            Print this help and exit\n\n"
        << "This is an early skeleton (Milestone 0): it loads and validates the\n"
        << "configuration, prints a summary, and exits. SQM polling, the database,\n"
        << "and the HTTP API arrive in later milestones.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires a path argument\n";
                return 2;
            }
            config_path = argv[++i];
        } else {
            std::cerr << "error: unknown argument '" << arg << "' (try --help)\n";
            return 2;
        }
    }

    if (config_path.empty()) {
        std::cerr << "error: no config file given (use --config <path>)\n";
        return 2;
    }

    try {
        const nightwatcher::Config cfg = nightwatcher::Config::load(config_path);
        std::cout << "nightwatcherd " << NIGHTWATCHER_VERSION << " starting\n"
                  << "  config file   : " << config_path << "\n"
                  << "  poll interval : " << cfg.poll_interval_s << " s\n"
                  << "  database      : " << cfg.db_user << "@" << cfg.db_host
                  << "/" << cfg.db_name << "\n"
                  << "  api port      : " << cfg.api_port << "\n"
                  << "  sensors       : " << cfg.sensors.size() << "\n";
        for (const auto& s : cfg.sensors) {
            std::cout << "    - " << s.id << " [" << s.transport << "] " << s.address
                      << (s.name.empty() ? "" : " (" + s.name + ")") << "\n";
        }
        std::cout << "\nSkeleton run complete \xe2\x80\x94 no polling yet (Milestone 0).\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
