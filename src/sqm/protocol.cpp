// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/sqm/protocol.cpp
// Purpose:       Implementation of the Unihedron SQM response parsers. Responses
//                are comma-separated fields with a trailing unit letter per
//                field; we split on ',' and strip the unit suffix.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nightwatcher::sqm {
namespace {

std::string trim(const std::string& s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto b = std::find_if(s.begin(), s.end(), not_space);
    const auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return (b < e) ? std::string(b, e) : std::string();
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream ss(s);
    while (std::getline(ss, item, delim)) out.push_back(item);
    return out;
}

// Strip surrounding whitespace and any trailing unit letters (e.g. "m", "Hz",
// "c", "s", "C") from a field, leaving the numeric token.
std::string numeric_part(std::string s) {
    s = trim(s);
    while (!s.empty() && std::isalpha(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return trim(s);
}

double to_double(const std::string& field) {
    const std::string n = numeric_part(field);
    if (n.empty()) throw std::runtime_error("expected number in field: '" + field + "'");
    return std::stod(n);
}

long long to_ll(const std::string& field) {
    const std::string n = numeric_part(field);
    if (n.empty()) throw std::runtime_error("expected integer in field: '" + field + "'");
    return std::stoll(n);
}

}  // namespace

Reading parse_reading(const std::string& line_in) {
    const std::string line = trim(line_in);
    const auto f = split(line, ',');
    // Reading responses start with 'r' (averaged/interval) or 'u' (unaveraged).
    if (f.size() < 6 || (trim(f[0]) != "r" && trim(f[0]) != "u")) {
        throw std::runtime_error("not a reading response: '" + line + "'");
    }
    Reading r;
    r.mag_arcsec2 = to_double(f[1]);
    r.freq_hz = to_ll(f[2]);
    r.period_counts = to_ll(f[3]);
    r.period_s = to_double(f[4]);
    r.temp_c = to_double(f[5]);
    if (f.size() >= 7) r.serial = trim(f[6]);  // interval report appends serial
    r.raw = line;
    return r;
}

Calibration parse_calibration(const std::string& line_in) {
    const std::string line = trim(line_in);
    const auto f = split(line, ',');
    if (f.size() < 6 || trim(f[0]) != "c") {
        throw std::runtime_error("not a calibration response: '" + line + "'");
    }
    Calibration c;
    c.light_cal_offset = to_double(f[1]);
    c.dark_cal_period_s = to_double(f[2]);
    c.temp_light_c = to_double(f[3]);
    c.sensor_offset = to_double(f[4]);
    c.temp_dark_c = to_double(f[5]);
    c.raw = line;
    return c;
}

UnitInfo parse_unit_info(const std::string& line_in) {
    const std::string line = trim(line_in);
    const auto f = split(line, ',');
    if (f.size() < 5 || trim(f[0]) != "i") {
        throw std::runtime_error("not a unit-info response: '" + line + "'");
    }
    UnitInfo u;
    u.protocol = static_cast<int>(to_ll(f[1]));
    u.model = static_cast<int>(to_ll(f[2]));
    u.feature = static_cast<int>(to_ll(f[3]));
    u.serial = trim(f[4]);
    u.raw = line;
    return u;
}

}  // namespace nightwatcher::sqm
