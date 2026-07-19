// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/cli/gdrive_auth.cpp
// Purpose:       One-time, headless OAuth2 helper: mint a Google Drive refresh
//                token by copy/pasting an authorization code. The consent step
//                runs in a browser on ANY device; this tool needs no GUI.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "gdrive.hpp"

namespace ex = nightwatcher::exporter;

namespace {

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// Accept either a bare code or a pasted redirect URL and return the code.
std::string extract_code(std::string in) {
    const auto p = in.find("code=");
    if (p != std::string::npos) {
        in = in.substr(p + 5);
        const auto amp = in.find('&');
        if (amp != std::string::npos) in = in.substr(0, amp);
    }
    while (!in.empty() && (in.back() == '\n' || in.back() == '\r' || in.back() == ' ')) in.pop_back();
    while (!in.empty() && in.front() == ' ') in.erase(in.begin());
    return url_decode(in);
}

}  // namespace

int main(int argc, char** argv) {
    std::string client_id, client_secret;
    std::string redirect = "http://127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--client-id" && i + 1 < argc) client_id = argv[++i];
        else if (a == "--client-secret" && i + 1 < argc) client_secret = argv[++i];
        else if (a == "--redirect" && i + 1 < argc) redirect = argv[++i];
        else if (a == "-h" || a == "--help") { client_id.clear(); break; }
    }
    if (client_id.empty() || client_secret.empty()) {
        std::fprintf(stderr,
                     "usage: nwexport-auth --client-id ID --client-secret SECRET [--redirect URI]\n\n"
                     "Mints a Google Drive refresh token for an OAuth \"Desktop app\" client.\n"
                     "Default redirect http://127.0.0.1 (allowed for Desktop clients).\n");
        return 2;
    }

    const std::string scope = "https://www.googleapis.com/auth/drive.file";
    std::cout << "\n1) Open this URL in a browser on ANY device and approve access:\n\n"
              << ex::oauth_consent_url(client_id, redirect, scope) << "\n\n"
              << "2) Your browser will land on a " << redirect
              << "/?code=... page that fails to load.\n"
                 "   That is expected on a headless setup. Copy the whole address (or just the\n"
                 "   code= value) from the address bar and paste it below.\n\n"
              << "Paste code (or the redirect URL): ";

    std::string line;
    std::getline(std::cin, line);
    const std::string code = extract_code(line);
    if (code.empty()) {
        std::fprintf(stderr, "no code provided\n");
        return 1;
    }

    try {
        const std::string refresh = ex::oauth_exchange_code(client_id, client_secret, code, redirect);
        if (refresh.empty()) {
            std::fprintf(stderr,
                         "\nNo refresh_token was returned. Re-run and make sure you approve the\n"
                         "consent screen (prompt=consent forces a fresh refresh token).\n");
            return 1;
        }
        std::cout << "\nSuccess. Add this to the export target's config under \"auth\":\n\n"
                  << "  {\n    \"mode\": \"oauth\",\n    \"client_id\": \"" << client_id << "\",\n"
                  << "    \"client_secret\": \"<your secret>\",\n    \"refresh_token\": \"" << refresh
                  << "\"\n  }\n\n"
                  << "(The daemon stores these in the target config; secrets are masked when read back.)\n";
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nauthorization failed: %s\n", e.what());
        return 1;
    }
}
