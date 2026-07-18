// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/discovery.cpp
// Purpose:       Implementation of subnet SQM discovery: expand a CIDR, then
//                probe each host concurrently with the ix command.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "discovery.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "sqm_device.hpp"
#include "tcp_transport.hpp"

namespace nightwatcher::sqm {
namespace {

uint32_t ipv4_to_u32(const std::string& s) {
    struct in_addr a {};
    if (::inet_pton(AF_INET, s.c_str(), &a) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + s);
    }
    return ntohl(a.s_addr);
}

std::string u32_to_ipv4(uint32_t v) {
    struct in_addr a {};
    a.s_addr = htonl(v);
    char buf[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

std::vector<std::string> cidr_hosts(const std::string& cidr) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        throw std::runtime_error("expected CIDR like 192.168.1.0/24: " + cidr);
    }
    const std::string base = cidr.substr(0, slash);
    const int prefix = std::stoi(cidr.substr(slash + 1));
    if (prefix < 0 || prefix > 32) {
        throw std::runtime_error("invalid CIDR prefix: " + cidr);
    }

    const uint32_t ip = ipv4_to_u32(base);
    const uint32_t mask = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    const uint32_t network = ip & mask;
    const uint32_t broadcast = network | ~mask;

    uint32_t first = network;
    uint32_t last = broadcast;
    if (prefix <= 30) {
        first = network + 1;
        last = broadcast - 1;
    }

    const uint64_t count = static_cast<uint64_t>(last) - first + 1;
    if (count > 65536) {
        throw std::runtime_error("subnet too large to scan (" + std::to_string(count) +
                                 " hosts); use a /16 or smaller range");
    }

    std::vector<std::string> hosts;
    hosts.reserve(static_cast<size_t>(count));
    for (uint32_t a = first;; ++a) {
        hosts.push_back(u32_to_ipv4(a));
        if (a == last) break;  // guard against wrap when last == 0xFFFFFFFF
    }
    return hosts;
}

std::vector<Discovered> discover(const std::string& cidr, uint16_t port, int timeout_ms,
                                 int concurrency) {
    const std::vector<std::string> hosts = cidr_hosts(cidr);

    std::vector<Discovered> results;
    std::mutex mtx;
    std::atomic<size_t> next{0};

    const int nthreads =
        std::max(1, std::min<int>(concurrency, static_cast<int>(hosts.size())));

    auto worker = [&]() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= hosts.size()) return;
            const std::string& ip = hosts[i];
            try {
                auto transport = std::make_unique<TcpTransport>(ip, port, timeout_ms);
                SqmDevice dev(std::move(transport));
                const UnitInfo info = dev.info();  // sends ix; throws if not an SQM
                std::lock_guard<std::mutex> lock(mtx);
                results.push_back(Discovered{ip, info});
            } catch (const std::exception&) {
                // Unreachable host or not an SQM — skip it.
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads));
    for (int i = 0; i < nthreads; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    std::sort(results.begin(), results.end(), [](const Discovered& a, const Discovered& b) {
        return ipv4_to_u32(a.ip) < ipv4_to_u32(b.ip);
    });
    return results;
}

}  // namespace nightwatcher::sqm
