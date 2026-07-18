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

}  // namespace nightwatcher::sqm
