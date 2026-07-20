// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/exporter.cpp
// Purpose:       Export-target factory + the Dark Sky Network exporter. Formats
//                readings to the community .dat and writes them to a destination
//                (local outbox now; Google Drive upload in a later stage).
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "exporter.hpp"
#include "gdrive.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace nightwatcher::exporter {
namespace {

// Split "https://host[:port]/path?query" into {"https://host[:port]", "/path?query"}.
std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto scheme = url.find("://");
    const auto slash = (scheme == std::string::npos) ? url.find('/') : url.find('/', scheme + 3);
    if (slash == std::string::npos) return {url, "/"};
    return {url.substr(0, slash), url.substr(slash)};
}

// Push a sensor's readings to the Dark Sky Network as a community-format .dat.
// Destination is chosen from config: `outbox_dir` writes locally; `drive_folder_id`
// uploads to Google Drive (wired in a later stage).
class DsnExporter : public IExporter {
public:
    explicit DsnExporter(std::string config) : config_(std::move(config)) {}

    ExportResult run(const ExportContext& ctx) override {
        const std::string content = format_dsn_dat(ctx, config_);
        ExportResult r;
        r.file_name = dsn_file_name(ctx, config_);
        r.row_count = static_cast<long long>(ctx.readings.size());

        const json cfg = config_.empty() ? json::object() : json::parse(config_);

        const std::string outbox = cfg.value("outbox_dir", std::string());
        if (!outbox.empty()) {
            const std::string path = outbox + "/" + r.file_name;
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f) throw std::runtime_error("cannot open export file for writing: " + path);
            f << content;
            if (!f) throw std::runtime_error("failed writing export file: " + path);
            r.remote_id = "file://" + path;
            return r;
        }

        const std::string folder = cfg.value("drive_folder_id", std::string());
        if (!folder.empty()) {
            DriveClient drive(drive_auth_from_config(config_));
            r.remote_id = drive.upload_or_update(folder, r.file_name, content, "text/plain");
            return r;
        }

        throw std::runtime_error("dsn export has no destination: set drive_folder_id or outbox_dir");
    }

private:
    std::string config_;
};

// POST readings as JSON to an HTTP(S) endpoint (e.g. the NightWatcher WordPress
// plugin's ingest route). Incremental: run_export hands it only the readings
// newer than the watermark, and the receiver de-dupes, so each is sent once.
class WebhookExporter : public IExporter {
public:
    explicit WebhookExporter(std::string config) : config_(std::move(config)) {}

    Windowing windowing() const override { return Windowing::Incremental; }

    ExportResult run(const ExportContext& ctx) override {
        const json cfg = config_.empty() ? json::object() : json::parse(config_);
        const std::string url = cfg.value("url", std::string());
        if (url.empty()) throw std::runtime_error("webhook export requires a 'url'");
        const std::string token = cfg.value("token", std::string());
        const std::string site_id = cfg.value("site_id", ctx.sensor.id);
        const bool insecure = cfg.value("insecure", false);
        int batch = cfg.value("batch", 2000);
        if (batch < 1) batch = 2000;

        // Sensor metadata, sent with every batch (the receiver upserts it).
        json sensor{{"id", ctx.sensor.id}};
        if (!ctx.sensor.name.empty()) sensor["name"] = ctx.sensor.name;
        sensor["latitude"] = ctx.sensor.latitude ? json(*ctx.sensor.latitude) : json(nullptr);
        sensor["longitude"] = ctx.sensor.longitude ? json(*ctx.sensor.longitude) : json(nullptr);
        sensor["elevation_m"] = ctx.sensor.elevation_m ? json(*ctx.sensor.elevation_m) : json(nullptr);
        if (!ctx.sensor.timezone.empty()) sensor["timezone"] = ctx.sensor.timezone;

        const auto [base, path] = split_url(url);
        httplib::Client cli(base);
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(30, 0);
        cli.set_write_timeout(30, 0);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (insecure) cli.enable_server_certificate_verification(false);
#endif
        httplib::Headers headers;
        if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);

        // Co-located ambient temperature for the whole window; the receiver
        // interpolates it onto the reading timestamps. Sent with each batch so
        // every POST is self-contained (the receiver de-dupes).
        json warr = json::array();
        for (const auto& w : ctx.weather)
            if (w.temp_c.has_value())
                warr.push_back({{"ts_utc", w.ts_utc}, {"temp_c", *w.temp_c}});

        const auto& rs = ctx.readings;
        for (size_t i = 0; i < rs.size(); i += static_cast<size_t>(batch)) {
            const size_t end = std::min(rs.size(), i + static_cast<size_t>(batch));
            json arr = json::array();
            for (size_t k = i; k < end; ++k) {
                arr.push_back({{"ts_utc", rs[k].ts_utc},
                               {"mag_arcsec2", rs[k].mag_arcsec2},
                               {"temp_c", rs[k].temp_c},
                               {"quality", rs[k].quality}});
            }
            json obj = {{"site_id", site_id}, {"sensor", sensor}, {"readings", arr}};
            if (!warr.empty()) obj["weather"] = warr;
            const std::string body = obj.dump();
            auto res = cli.Post(path.c_str(), headers, body, "application/json");
            if (!res)
                throw std::runtime_error("webhook POST to " + url + " failed: " +
                                         httplib::to_string(res.error()));
            if (res->status < 200 || res->status >= 300)
                throw std::runtime_error("webhook POST returned HTTP " +
                                         std::to_string(res->status) + ": " + res->body);
        }

        ExportResult r;
        r.file_name = "webhook";
        r.remote_id = url;
        r.row_count = static_cast<long long>(rs.size());
        return r;
    }

private:
    std::string config_;
};

}  // namespace

std::unique_ptr<IExporter> make_exporter(const std::string& target, const std::string& config_json) {
    if (!config_json.empty()) {
        try {
            [[maybe_unused]] const auto parsed = json::parse(config_json);
        } catch (const std::exception&) {
            throw std::runtime_error(target + ": invalid config JSON");
        }
    }
    if (target == "dsn") return std::make_unique<DsnExporter>(config_json);
    if (target == "webhook") return std::make_unique<WebhookExporter>(config_json);
    if (target == "globeatnight")
        throw std::runtime_error("globeatnight exporter is not implemented yet");
    throw std::runtime_error("unknown export target: " + target);
}

}  // namespace nightwatcher::exporter
