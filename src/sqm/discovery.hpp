// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/discovery.hpp
// Purpose:       Find SQM-LE units on a subnet by probing <ip>:port with the
//                unit-information command (ix) and keeping the responders.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace nightwatcher::sqm {

// An SQM found during a scan: its IP plus the parsed unit-info response.
struct Discovered {
    std::string ip;
    UnitInfo info;
};

// An SQM-LU found on the USB/serial bus: its device path (a stable
// /dev/serial/by-id/... name when available) plus the parsed unit-info.
struct DiscoveredSerial {
    std::string device;
    UnitInfo info;
};

// Expand an IPv4 CIDR (e.g. "192.168.1.0/24") into candidate host addresses.
// Network and broadcast addresses are excluded for prefixes <= 30; /31 and /32
// are returned inclusive. Throws std::runtime_error on a malformed CIDR or a
// range larger than 65536 hosts.
std::vector<std::string> cidr_hosts(const std::string& cidr);

// Scan every host in `cidr`, concurrently, for an SQM-LE listening on `port`.
// A host qualifies only if it accepts a TCP connection AND returns a valid
// unit-information (ix) response, so non-SQM services are not misreported.
// Results are sorted by ascending IP. This is a robust, protocol-verifying
// alternative to Lantronix UDP broadcast discovery.
std::vector<Discovered> discover(const std::string& cidr, uint16_t port = 10001,
                                 int timeout_ms = 700, int concurrency = 128);

// Probe the local serial/USB bus for SQM-LU units: enumerate serial devices
// (stable /dev/serial/by-id/ names, plus /dev/ttyUSB* and /dev/ttyACM*), open
// each at 115200 8N1, and keep those that return a valid ix response. Devices
// held by another process (e.g. a live poll) are skipped. Results are sorted by
// device path. Note: this briefly opens and sends `ix` to each serial port.
std::vector<DiscoveredSerial> discover_serial(int timeout_ms = 2000);

}  // namespace nightwatcher::sqm
