// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/export_test.cpp
// Purpose:       Unit tests for the DSN .dat formatter + export factory. Checks
//                structure and data correctness (incl. UTC->local conversion)
//                rather than byte-exact formatting, which is reconciled against a
//                real DSN sample. No database or network required.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <unistd.h>  // rmdir

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "exporter.hpp"

namespace ex = nightwatcher::exporter;
namespace db = nightwatcher::db;

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n",         \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static ex::ExportContext make_ctx() {
    db::SensorRow s;
    s.id = "DSN036";
    s.name = "Sabino";
    s.site = "Tucson";
    s.model = "SQM-LE";
    s.serial_number = "00007141";
    s.feature_ver = 44;
    s.latitude = 32.2500;
    s.longitude = -110.9000;
    s.elevation_m = 750.0;
    s.timezone = "America/Phoenix";  // fixed UTC-7, no DST

    ex::ExportContext ctx;
    ctx.sensor = s;
    db::ReadingRow r1;
    r1.sensor_id = "DSN036";
    r1.ts_utc = "2026-07-19 04:00:00";
    r1.mag_arcsec2 = 20.13;
    r1.freq_hz = 123;
    r1.period_counts = 456;
    r1.temp_c = 15.5;
    db::ReadingRow r2 = r1;
    r2.ts_utc = "2026-07-19 04:05:00";
    r2.mag_arcsec2 = 20.20;
    ctx.readings = {r1, r2};
    ctx.from_ts = "2026-07-19 00:00:00";
    ctx.to_ts = "2026-07-19 23:59:59";
    return ctx;
}

int main() {
    const auto ctx = make_ctx();
    const std::string dat = ex::format_dsn_dat(ctx, "{}");
    const auto lines = split_lines(dat);

    // --- Header structure ---
    CHECK(!lines.empty());
    CHECK(lines.front() == "# Light Pollution Monitoring Data Format 1.0");

    int header_lines = 0;
    int declared = -1;
    bool saw_end = false;
    for (const auto& l : lines) {
        if (!l.empty() && l[0] == '#') {
            ++header_lines;
            const std::string key = "# Number of header lines: ";
            if (l.rfind(key, 0) == 0) declared = std::atoi(l.c_str() + key.size());
            if (contains(l, "END OF HEADER")) saw_end = true;
        }
    }
    CHECK(saw_end);
    // The self-referential count must equal the actual number of header lines.
    CHECK(declared == header_lines);

    CHECK(contains(dat, "# Instrument ID: DSN036"));
    CHECK(contains(dat, "# Position (lat, lon, elev(m)): 32.250000, -110.900000, 750.0"));
    CHECK(contains(dat, "# Local timezone: America/Phoenix"));
    CHECK(contains(dat, "# SQM serial number: 00007141"));

    // --- Data rows ---
    std::vector<std::string> data;
    for (const auto& l : lines)
        if (!l.empty() && l[0] != '#') data.push_back(l);
    CHECK(data.size() == 2);

    // First data row: 6 semicolon-separated fields.
    std::vector<std::string> f;
    {
        std::istringstream in(data.at(0));
        std::string tok;
        while (std::getline(in, tok, ';')) f.push_back(tok);
    }
    CHECK(f.size() == 6);
    if (f.size() == 6) {
        CHECK(f[0] == "2026-07-19T04:00:00.000");        // UTC column
        CHECK(f[1] == "2026-07-18T21:00:00.000");        // local = UTC-7 (Phoenix)
        CHECK(f[2] == "15.5");                            // temperature
        CHECK(f[3] == "456");                            // counts
        CHECK(f[4] == "123");                            // frequency
        CHECK(f[5] == "20.13");                          // MSAS
    }

    // --- File name ---
    CHECK(ex::dsn_file_name(ctx, R"({"site_id":"DSN036-S"})") == "DSN036-S_20260719.dat");
    CHECK(ex::dsn_file_name(ctx, "{}") == "DSN036_20260719.dat");

    // --- Factory ---
    bool threw = false;
    try { ex::make_exporter("nope", "{}"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { ex::make_exporter("dsn", "{not json"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(ex::make_exporter("dsn", R"({"site_id":"DSN036-S"})") != nullptr);

    // --- Outbox round-trip (run() writes the .dat locally) ---
    char tmpl[] = "/tmp/nwexport_XXXXXX";
    if (char* dir = mkdtemp(tmpl)) {
        const std::string cfg = std::string(R"({"site_id":"DSN036-S","outbox_dir":")") + dir + "\"}";
        auto exp = ex::make_exporter("dsn", cfg);
        const ex::ExportResult res = exp->run(ctx);
        CHECK(res.row_count == 2);
        CHECK(res.file_name == "DSN036-S_20260719.dat");
        CHECK(res.remote_id.rfind("file://", 0) == 0);
        const std::string path = std::string(dir) + "/" + res.file_name;
        std::ifstream f2(path, std::ios::binary);
        CHECK(f2.good());
        std::stringstream ss;
        ss << f2.rdbuf();
        const std::string written = ss.str();
        CHECK(written.rfind("# Light Pollution Monitoring Data Format 1.0", 0) == 0);
        f2.close();
        std::remove(path.c_str());
        rmdir(dir);
    } else {
        std::puts("export_test: skipped outbox round-trip (mkdtemp failed)");
    }

    if (g_failures == 0) {
        std::puts("export_test passed");
        return 0;
    }
    std::fprintf(stderr, "export_test FAILED (%d checks)\n", g_failures);
    return 1;
}
