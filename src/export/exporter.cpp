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
#include <fstream>
#include <stdexcept>
#include <string>

#include "exporter.hpp"
#include "gdrive.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace nightwatcher::exporter {
namespace {

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
    if (target == "globeatnight")
        throw std::runtime_error("globeatnight exporter is not implemented yet");
    throw std::runtime_error("unknown export target: " + target);
}

}  // namespace nightwatcher::exporter
