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
#include <thread>
#include <vector>

#include "exporter.hpp"
#include "httplib.h"

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
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF -> strip CR
        out.push_back(line);
    }
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
    s.latitude = 31.9500;   // obfuscated placeholder (not a real site)
    s.longitude = -111.6000;
    s.elevation_m = 1200.0;
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

    // --- Header structure (Community Standard Skyglow Data Format, 35 lines) ---
    CHECK(!lines.empty());
    CHECK(lines.front() == "# Community Standard Skyglow Data Format 1.0");
    CHECK(dat.find("\r\n") != std::string::npos);  // CRLF terminators

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
    CHECK(header_lines == 35);
    CHECK(declared == 35);

    CHECK(contains(dat, "# Position (lat, lon, elev(m)): +31.9500, -111.6000, +1200"));
    CHECK(contains(dat, "# Local timezone: America/Phoenix"));
    CHECK(contains(dat, "# SQM serial number: 7141"));  // leading zeros stripped
    CHECK(contains(dat, "# Number of fields per line: 6"));

    // --- Data rows ---
    std::vector<std::string> data;
    for (const auto& l : lines)
        if (!l.empty() && l[0] != '#') data.push_back(l);
    CHECK(data.size() == 2);

    // First data row: 6 semicolon-separated fields; Counts+Frequency empty.
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
        CHECK(f[3].empty());                             // counts blank by default
        CHECK(f[4].empty());                             // frequency blank by default
        CHECK(f[5] == "20.13");                          // MSAS
    }
    // Second row's MSAS strips the trailing zero (20.20 -> "20.2").
    CHECK(contains(data.at(1), ";20.2"));

    // Opt-in Counts+Frequency populate from the device values.
    {
        const std::string dat_cf = ex::format_dsn_dat(ctx, R"({"include_counts_freq":true})");
        CHECK(contains(dat_cf, ";15.5;456;123;20.13"));
    }

    // --- File name (site[_supplier]_<local YY-MM>.dat) ---
    CHECK(ex::dsn_file_name(ctx, R"({"site_id":"DSN036-S","supplier":"Gilinsky"})") ==
          "DSN036-S_Gilinsky_26-07.dat");
    CHECK(ex::dsn_file_name(ctx, "{}") == "DSN036_26-07.dat");

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
        CHECK(res.file_name == "DSN036-S_26-07.dat");
        CHECK(res.remote_id.rfind("file://", 0) == 0);
        const std::string path = std::string(dir) + "/" + res.file_name;
        std::ifstream f2(path, std::ios::binary);
        CHECK(f2.good());
        std::stringstream ss;
        ss << f2.rdbuf();
        const std::string written = ss.str();
        CHECK(written.rfind("# Community Standard Skyglow Data Format 1.0", 0) == 0);
        f2.close();
        std::remove(path.c_str());
        rmdir(dir);
    } else {
        std::puts("export_test: skipped outbox round-trip (mkdtemp failed)");
    }

    // --- Webhook exporter: POST readings to a local receiver ---
    {
        // Missing url -> run() throws.
        auto bad = ex::make_exporter("webhook", "{}");
        threw = false;
        try { bad->run(ctx); } catch (const std::exception&) { threw = true; }
        CHECK(threw);

        // The webhook exporter is incremental, not month-rebuild.
        CHECK(ex::make_exporter("webhook", R"({"url":"http://x/y"})")->windowing() ==
              ex::Windowing::Incremental);

        httplib::Server srv;
        int posts = 0;
        std::string all_bodies, last_auth;
        srv.Post("/ingest", [&](const httplib::Request& req, httplib::Response& res) {
            ++posts;
            last_auth = req.get_header_value("Authorization");
            all_bodies += req.body;
            res.set_content(R"({"ok":true,"stored":1})", "application/json");
        });
        // A 401 endpoint to prove non-2xx surfaces as an error.
        srv.Post("/deny", [](const httplib::Request&, httplib::Response& res) {
            res.status = 401;
            res.set_content("nope", "text/plain");
        });

        const int port = 18110;
        if (srv.bind_to_port("127.0.0.1", port)) {
            std::thread th([&] { srv.listen_after_bind(); });
            const std::string base = "http://127.0.0.1:" + std::to_string(port);

            // batch=1 with 2 readings -> two POSTs, each bearer-authed.
            const std::string cfg = R"({"url":")" + base +
                R"(/ingest","token":"s3cret","site_id":"DSN036-S","batch":1})";
            const ex::ExportResult wres = ex::make_exporter("webhook", cfg)->run(ctx);
            CHECK(wres.row_count == 2);
            CHECK(wres.remote_id == base + "/ingest");
            CHECK(posts == 2);                                   // chunked into 2
            CHECK(last_auth == "Bearer s3cret");
            CHECK(contains(all_bodies, "\"site_id\":\"DSN036-S\""));
            CHECK(contains(all_bodies, "2026-07-19 04:00:00"));  // reading 1
            CHECK(contains(all_bodies, "2026-07-19 04:05:00"));  // reading 2
            CHECK(contains(all_bodies, "\"latitude\":31.95"));   // sensor metadata

            // A non-2xx response must throw.
            threw = false;
            try {
                ex::make_exporter("webhook", R"({"url":")" + base + R"(/deny"})")->run(ctx);
            } catch (const std::exception&) { threw = true; }
            CHECK(threw);

            srv.stop();
            th.join();
        } else {
            std::puts("export_test: skipped webhook test (bind failed)");
        }
    }

    if (g_failures == 0) {
        std::puts("export_test passed");
        return 0;
    }
    std::fprintf(stderr, "export_test FAILED (%d checks)\n", g_failures);
    return 1;
}
