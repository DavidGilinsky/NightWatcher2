// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/gdrive.cpp
// Purpose:       Google Drive upload client: service-account (JWT RS256) and
//                OAuth2 refresh-token auth, plus create-or-update-by-name upload.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include "gdrive.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cctype>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace nightwatcher::exporter {
namespace {

std::string url_encode(const std::string& s) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0f];
        }
    }
    return out;
}

std::string b64(const unsigned char* data, size_t len) {
    if (len == 0) return "";
    std::string out;
    out.resize(4 * ((len + 2) / 3));
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    out.resize(n < 0 ? 0 : static_cast<size_t>(n));
    return out;
}

// RS256 signature of `data` using a PEM private key. Throws on a bad key.
std::string rs256_sign(const std::string& data, const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) throw std::runtime_error("out of memory");
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) throw std::runtime_error("invalid service-account private key (PEM expected)");

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::string sig;
    bool ok = false;
    if (ctx && EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSignUpdate(ctx, data.data(), data.size()) == 1) {
        size_t len = 0;
        if (EVP_DigestSignFinal(ctx, nullptr, &len) == 1) {
            sig.resize(len);
            if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len) == 1) {
                sig.resize(len);
                ok = true;
            }
        }
    }
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok) throw std::runtime_error("RS256 signing failed");
    return sig;
}

// Split "https://host/path?query" into {"https://host", "/path?query"}.
std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto p = url.find("://");
    const size_t start = (p == std::string::npos) ? 0 : p + 3;
    const auto slash = url.find('/', start);
    if (slash == std::string::npos) return {url, "/"};
    return {url.substr(0, slash), url.substr(slash)};
}

void check(const httplib::Result& res, const std::string& what) {
    if (!res)
        throw std::runtime_error(what + " failed: " + httplib::to_string(res.error()));
    if (res->status < 200 || res->status >= 300)
        throw std::runtime_error(what + ": HTTP " + std::to_string(res->status) + " " +
                                 res->body.substr(0, 300));
}

// POST an application/x-www-form-urlencoded body and return the parsed JSON.
json post_form(const std::string& url, const std::string& body) {
    const auto [base, path] = split_url(url);
    httplib::Client cli(base);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(20, 0);
    auto res = cli.Post(path.c_str(), body, "application/x-www-form-urlencoded");
    check(res, "token request to " + base);
    return json::parse(res->body);
}

}  // namespace

std::string base64url(const std::string& data) {
    std::string s = b64(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string make_service_account_jwt(const std::string& client_email, const std::string& private_key_pem,
                                     const std::string& scope, const std::string& aud, long iat, long exp) {
    const json header = {{"alg", "RS256"}, {"typ", "JWT"}};
    const json claims = {{"iss", client_email}, {"scope", scope}, {"aud", aud}, {"iat", iat}, {"exp", exp}};
    const std::string signing_input = base64url(header.dump()) + "." + base64url(claims.dump());
    const std::string sig = rs256_sign(signing_input, private_key_pem);
    return signing_input + "." + base64url(sig);
}

DriveClient::DriveClient(DriveAuth auth) : auth_(std::move(auth)) {}

std::string DriveClient::access_token() {
    if (auth_.mode == "service_account") {
        const long now = static_cast<long>(std::time(nullptr));
        const std::string jwt =
            make_service_account_jwt(auth_.client_email, auth_.private_key,
                                     "https://www.googleapis.com/auth/drive", auth_.token_uri,
                                     now, now + 3600);
        const std::string body =
            "grant_type=" + url_encode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
            "&assertion=" + url_encode(jwt);
        const json j = post_form(auth_.token_uri, body);
        if (!j.contains("access_token"))
            throw std::runtime_error("service-account token response missing access_token: " +
                                     j.dump().substr(0, 200));
        return j["access_token"].get<std::string>();
    }
    if (auth_.mode == "oauth") {
        const std::string body = "client_id=" + url_encode(auth_.client_id) + "&client_secret=" +
                                 url_encode(auth_.client_secret) + "&refresh_token=" +
                                 url_encode(auth_.refresh_token) + "&grant_type=refresh_token";
        const json j = post_form(auth_.token_uri, body);
        if (!j.contains("access_token"))
            throw std::runtime_error("oauth token response missing access_token: " +
                                     j.dump().substr(0, 200));
        return j["access_token"].get<std::string>();
    }
    throw std::runtime_error("drive auth mode must be service_account or oauth");
}

std::string DriveClient::upload_or_update(const std::string& folder_id, const std::string& name,
                                          const std::string& content, const std::string& mime) {
    const std::string token = access_token();
    const httplib::Headers auth_hdr = {{"Authorization", "Bearer " + token}};
    httplib::Client api("https://www.googleapis.com");
    api.set_connection_timeout(10, 0);
    api.set_read_timeout(30, 0);

    // Find an existing file of this name in the folder.
    const std::string q =
        "name = '" + name + "' and '" + folder_id + "' in parents and trashed = false";
    const std::string find = "/drive/v3/files?q=" + url_encode(q) +
                             "&fields=" + url_encode("files(id,name)") + "&spaces=drive" +
                             "&supportsAllDrives=true&includeItemsFromAllDrives=true";
    auto sres = api.Get(find.c_str(), auth_hdr);
    check(sres, "drive file search");
    const json sj = json::parse(sres->body);
    std::string existing;
    if (sj.contains("files") && sj["files"].is_array() && !sj["files"].empty())
        existing = sj["files"][0].value("id", std::string());

    if (!existing.empty()) {
        // Update the content of the existing file (media upload).
        const std::string up =
            "/upload/drive/v3/files/" + existing + "?uploadType=media&supportsAllDrives=true";
        auto ures = api.Patch(up.c_str(), auth_hdr, content, mime.c_str());
        check(ures, "drive file update");
        return existing;
    }

    // Create a new file (multipart: JSON metadata + media).
    const std::string boundary = "nwexport_boundary_dsn";
    const json meta = {{"name", name}, {"parents", json::array({folder_id})}};
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
    body += meta.dump() + "\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Type: " + mime + "\r\n\r\n";
    body += content + "\r\n";
    body += "--" + boundary + "--";
    const std::string ct = "multipart/related; boundary=" + boundary;
    auto cres = api.Post("/upload/drive/v3/files?uploadType=multipart&supportsAllDrives=true", auth_hdr,
                         body, ct.c_str());
    check(cres, "drive file create");
    const json cj = json::parse(cres->body);
    return cj.value("id", std::string());
}

DriveAuth drive_auth_from_config(const std::string& config_json) {
    const json cfg = config_json.empty() ? json::object() : json::parse(config_json);
    const json a = (cfg.contains("auth") && cfg["auth"].is_object()) ? cfg["auth"] : cfg;

    DriveAuth d;
    d.mode = a.value("mode", std::string());
    if (d.mode.empty()) {  // infer from the fields present
        if (a.contains("private_key")) d.mode = "service_account";
        else if (a.contains("refresh_token")) d.mode = "oauth";
    }
    if (a.contains("token_uri") && a["token_uri"].is_string() && !a["token_uri"].get<std::string>().empty())
        d.token_uri = a["token_uri"].get<std::string>();

    if (d.mode == "service_account") {
        d.client_email = a.value("client_email", std::string());
        d.private_key = a.value("private_key", std::string());
        if (d.client_email.empty() || d.private_key.empty())
            throw std::runtime_error("service_account auth requires client_email and private_key");
    } else if (d.mode == "oauth") {
        d.client_id = a.value("client_id", std::string());
        d.client_secret = a.value("client_secret", std::string());
        d.refresh_token = a.value("refresh_token", std::string());
        if (d.client_id.empty() || d.client_secret.empty() || d.refresh_token.empty())
            throw std::runtime_error("oauth auth requires client_id, client_secret, and refresh_token");
    } else {
        throw std::runtime_error("drive auth: set \"mode\" to service_account or oauth");
    }
    return d;
}

std::string oauth_consent_url(const std::string& client_id, const std::string& redirect_uri,
                              const std::string& scope) {
    return "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + url_encode(client_id) +
           "&redirect_uri=" + url_encode(redirect_uri) + "&response_type=code&scope=" +
           url_encode(scope) + "&access_type=offline&prompt=consent";
}

std::string oauth_exchange_code(const std::string& client_id, const std::string& client_secret,
                                const std::string& code, const std::string& redirect_uri) {
    const std::string body = "code=" + url_encode(code) + "&client_id=" + url_encode(client_id) +
                             "&client_secret=" + url_encode(client_secret) + "&redirect_uri=" +
                             url_encode(redirect_uri) + "&grant_type=authorization_code";
    const json j = post_form("https://oauth2.googleapis.com/token", body);
    return j.value("refresh_token", std::string());
}

}  // namespace nightwatcher::exporter
