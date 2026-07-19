// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/export/gdrive.hpp
// Purpose:       Google Drive upload client for the DSN exporter. Supports two
//                headless auth modes (service-account JWT and OAuth2
//                refresh-token) and creates-or-updates a file by name in a
//                folder so repeated monthly exports overwrite instead of
//                duplicating.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace nightwatcher::exporter {

// Google auth parameters parsed from an export target's config.
struct DriveAuth {
    std::string mode;          // "service_account" | "oauth"
    // service_account (a Google service-account JSON key supplies these):
    std::string client_email;
    std::string private_key;   // PEM
    // oauth (user delegation):
    std::string client_id;
    std::string client_secret;
    std::string refresh_token;
    std::string token_uri = "https://oauth2.googleapis.com/token";
};

// Uploads files to a Google Drive folder. Acquires an access token per the auth
// mode, then creates-or-updates a file by name in the folder. Throws on error.
class DriveClient {
public:
    explicit DriveClient(DriveAuth auth);
    std::string upload_or_update(const std::string& folder_id, const std::string& name,
                                 const std::string& content,
                                 const std::string& mime = "text/plain");

private:
    std::string access_token();
    DriveAuth auth_;
};

// Parse the Drive auth from a DSN target config's "auth" object (or the config
// root). Infers the mode from the fields present. Throws if incomplete.
DriveAuth drive_auth_from_config(const std::string& config_json);

// --- OAuth2 copy/paste helpers (used by the one-time auth CLI) ---
// Consent URL to open in a browser on any device (access_type=offline).
std::string oauth_consent_url(const std::string& client_id, const std::string& redirect_uri,
                              const std::string& scope);
// Exchange an authorization code for a refresh token. Returns the refresh token.
std::string oauth_exchange_code(const std::string& client_id, const std::string& client_secret,
                                const std::string& code, const std::string& redirect_uri);

// --- exposed for unit tests (no network) ---
std::string base64url(const std::string& data);
std::string make_service_account_jwt(const std::string& client_email,
                                      const std::string& private_key_pem, const std::string& scope,
                                      const std::string& aud, long iat, long exp);

}  // namespace nightwatcher::exporter
