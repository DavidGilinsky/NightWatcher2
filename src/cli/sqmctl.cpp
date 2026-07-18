// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/sqmctl.cpp
// Purpose:       Command-line tool for a Unihedron SQM: discover units on a
//                subnet, or query one over TCP (info/read/unaveraged/cal).
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "discovery.hpp"
#include "nightwatcher/version.hpp"
#include "protocol.hpp"
#include "sqm_device.hpp"
#include "tcp_transport.hpp"

namespace {

using namespace nightwatcher::sqm;

void usage(const char* argv0) {
    std::cout
        << "sqmctl " << NIGHTWATCHER_VERSION << " \xe2\x80\x94 Unihedron SQM tool\n\n"
        << "Usage:\n"
        << "  " << argv0 << " discover <CIDR> [--port N] [--timeout MS] [--concurrency N]\n"
        << "  " << argv0 << " --tcp HOST[:PORT] <command> [--timeout MS]\n\n"
        << "Query commands:\n"
        << "  info         Unit information (ix)\n"
        << "  read         Averaged reading (rx)\n"
        << "  unaveraged   Unaveraged reading (ux)\n"
        << "  cal          Calibration information (cx)\n\n"
        << "discover scans a subnet for SQM-LE units, e.g.:\n"
        << "  " << argv0 << " discover 192.168.1.0/24\n\n"
        << "Default port is 10001 (SQM-LE). Query timeout defaults to 5000 ms;\n"
        << "discovery timeout defaults to 700 ms with 128-way concurrency.\n";
}

void print_reading(const Reading& r) {
    std::cout << "raw           : " << r.raw << "\n"
              << "mag/arcsec^2  : " << r.mag_arcsec2 << "\n"
              << "frequency Hz  : " << r.freq_hz << "\n"
              << "period counts : " << r.period_counts << "\n"
              << "period s      : " << r.period_s << "\n"
              << "temperature C : " << r.temp_c << "\n";
    if (!r.serial.empty()) std::cout << "serial        : " << r.serial << "\n";
}

int run_discover(const std::string& cidr, uint16_t port, int timeout_ms, int concurrency) {
    try {
        const auto hosts = cidr_hosts(cidr);
        std::cerr << "scanning " << hosts.size() << " hosts on " << cidr << " (port " << port
                  << ", timeout " << timeout_ms << " ms, concurrency " << concurrency
                  << ") ...\n";
        const auto found = discover(cidr, port, timeout_ms, concurrency);
        if (found.empty()) {
            std::cout << "No SQM found on " << cidr << "\n";
            return 1;
        }
        std::cout << "Found " << found.size() << " SQM(s):\n";
        for (const auto& d : found) {
            std::cout << "  " << d.ip << ":" << port << "  serial=" << d.info.serial
                      << " model=" << d.info.model << " feature=" << d.info.feature
                      << " protocol=" << d.info.protocol << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

int run_query(const std::string& endpoint, const std::string& command, int timeout_ms) {
    std::string host = endpoint;
    uint16_t port = 10001;
    const auto colon = endpoint.rfind(':');
    if (colon != std::string::npos) {
        host = endpoint.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(endpoint.substr(colon + 1).c_str()));
    }

    try {
        auto transport = std::make_unique<TcpTransport>(host, port, timeout_ms);
        SqmDevice dev(std::move(transport));

        if (command == "info") {
            const UnitInfo u = dev.info();
            std::cout << "raw      : " << u.raw << "\n"
                      << "protocol : " << u.protocol << "\n"
                      << "model    : " << u.model << "\n"
                      << "feature  : " << u.feature << "\n"
                      << "serial   : " << u.serial << "\n";
        } else if (command == "read") {
            print_reading(dev.read_averaged());
        } else if (command == "unaveraged") {
            print_reading(dev.read_unaveraged());
        } else if (command == "cal") {
            const Calibration c = dev.calibration();
            std::cout << "raw               : " << c.raw << "\n"
                      << "light cal offset  : " << c.light_cal_offset << " mag\n"
                      << "dark cal period s : " << c.dark_cal_period_s << "\n"
                      << "temp @ light cal  : " << c.temp_light_c << " C\n"
                      << "sensor offset     : " << c.sensor_offset << " mag\n"
                      << "temp @ dark cal   : " << c.temp_dark_c << " C\n";
        } else {
            std::cerr << "error: unknown command '" << command << "'\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string endpoint;
    std::vector<std::string> positionals;
    int timeout_ms = -1;   // -1 => use a per-mode default
    uint16_t port = 10001;
    int concurrency = 128;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--version") {
            std::cout << "sqmctl " << NIGHTWATCHER_VERSION << "\n";
            return 0;
        } else if (a == "--tcp") {
            if (++i >= argc) { std::cerr << "error: --tcp requires HOST[:PORT]\n"; return 2; }
            endpoint = argv[i];
        } else if (a == "--timeout") {
            if (++i >= argc) { std::cerr << "error: --timeout requires MS\n"; return 2; }
            timeout_ms = std::atoi(argv[i]);
        } else if (a == "--port") {
            if (++i >= argc) { std::cerr << "error: --port requires N\n"; return 2; }
            port = static_cast<uint16_t>(std::atoi(argv[i]));
        } else if (a == "--concurrency") {
            if (++i >= argc) { std::cerr << "error: --concurrency requires N\n"; return 2; }
            concurrency = std::atoi(argv[i]);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option '" << a << "'\n";
            return 2;
        } else {
            positionals.push_back(a);
        }
    }

    if (positionals.empty()) {
        usage(argv[0]);
        return 2;
    }

    const std::string& command = positionals[0];

    if (command == "discover") {
        if (positionals.size() < 2) {
            std::cerr << "error: discover requires a CIDR, e.g. 192.168.1.0/24\n";
            return 2;
        }
        return run_discover(positionals[1], port, timeout_ms < 0 ? 700 : timeout_ms,
                            concurrency);
    }

    if (endpoint.empty()) {
        std::cerr << "error: '" << command << "' requires --tcp HOST[:PORT]\n";
        return 2;
    }
    return run_query(endpoint, command, timeout_ms < 0 ? 5000 : timeout_ms);
}
