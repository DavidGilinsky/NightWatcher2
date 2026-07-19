// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/gdrive_test.cpp
// Purpose:       Unit tests for the Google Drive auth helpers: base64url, config
//                parsing, OAuth consent URL, and service-account JWT signing
//                (verified with a freshly generated RSA key). No network.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdio>
#include <stdexcept>
#include <string>

#include "gdrive.hpp"

namespace ex = nightwatcher::exporter;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

static std::string base64url_decode(std::string s) {
    for (auto& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (s.size() % 4) s += '=';
    std::string out;
    out.resize(s.size());
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(s.data()),
                                  static_cast<int>(s.size()));
    if (n < 0) return "";
    int pad = 0;
    if (!s.empty() && s.back() == '=') ++pad;
    if (s.size() >= 2 && s[s.size() - 2] == '=') ++pad;
    out.resize(static_cast<size_t>(n) - pad);
    return out;
}

int main() {
    // --- base64url (RFC 4648 §5, no padding) ---
    CHECK(ex::base64url("") == "");
    CHECK(ex::base64url("f") == "Zg");
    CHECK(ex::base64url("fo") == "Zm8");
    CHECK(ex::base64url("foo") == "Zm9v");
    CHECK(ex::base64url("foobar") == "Zm9vYmFy");
    // url-safe: bytes that would yield '+' and '/' in standard base64.
    CHECK(ex::base64url(std::string("\xfb\xff\xbf", 3)) == "-_-_");

    // --- config parsing ---
    {
        const auto sa = ex::drive_auth_from_config(
            R"({"drive_folder_id":"X","auth":{"client_email":"a@b.iam","private_key":"KEY"}})");
        CHECK(sa.mode == "service_account");
        CHECK(sa.client_email == "a@b.iam");
        const auto oa = ex::drive_auth_from_config(
            R"({"auth":{"mode":"oauth","client_id":"c","client_secret":"s","refresh_token":"r"}})");
        CHECK(oa.mode == "oauth");
        CHECK(oa.refresh_token == "r");
        // auth at config root (not nested) also works.
        const auto flat = ex::drive_auth_from_config(R"({"client_id":"c","client_secret":"s","refresh_token":"r"})");
        CHECK(flat.mode == "oauth");
    }
    // error paths
    {
        bool t = false;
        try { ex::drive_auth_from_config(R"({"auth":{"private_key":"K"}})"); } catch (const std::exception&) { t = true; }
        CHECK(t);  // missing client_email
        t = false;
        try { ex::drive_auth_from_config(R"({"auth":{"mode":"oauth","client_id":"c"}})"); } catch (const std::exception&) { t = true; }
        CHECK(t);  // missing secret/refresh_token
        t = false;
        try { ex::drive_auth_from_config(R"({"foo":"bar"})"); } catch (const std::exception&) { t = true; }
        CHECK(t);  // no auth fields at all
    }

    // --- OAuth consent URL ---
    {
        const std::string u = ex::oauth_consent_url("CID.apps", "http://127.0.0.1",
                                                    "https://www.googleapis.com/auth/drive.file");
        CHECK(contains(u, "client_id=CID.apps"));
        CHECK(contains(u, "access_type=offline"));
        CHECK(contains(u, "response_type=code"));
        CHECK(contains(u, "drive.file"));
    }

    // --- service-account JWT: sign with a fresh RSA key, verify structure + signature ---
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    CHECK(pkey != nullptr);
    if (pkey) {
        BIO* bio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        char* data = nullptr;
        const long len = BIO_get_mem_data(bio, &data);
        const std::string pem(data, static_cast<size_t>(len));
        BIO_free(bio);

        const std::string jwt = ex::make_service_account_jwt(
            "svc@proj.iam.gserviceaccount.com", pem, "https://www.googleapis.com/auth/drive.file",
            "https://oauth2.googleapis.com/token", 1000, 4600);

        // Three non-empty parts.
        const auto d1 = jwt.find('.');
        const auto d2 = jwt.rfind('.');
        CHECK(d1 != std::string::npos && d2 != std::string::npos && d1 != d2);
        // Header encodes the expected algorithm.
        CHECK(jwt.rfind(ex::base64url(R"({"alg":"RS256","typ":"JWT"})") + ".", 0) == 0);
        // Claims decode to the expected issuer + audience.
        const std::string claims = base64url_decode(jwt.substr(d1 + 1, d2 - d1 - 1));
        CHECK(contains(claims, "\"iss\":\"svc@proj.iam.gserviceaccount.com\""));
        CHECK(contains(claims, "\"aud\":\"https://oauth2.googleapis.com/token\""));

        // Signature verifies against the key (proves valid RS256 Google will accept).
        const std::string signing_input = jwt.substr(0, d2);
        const std::string sig = base64url_decode(jwt.substr(d2 + 1));
        EVP_MD_CTX* vctx = EVP_MD_CTX_new();
        int rc = -1;
        if (vctx && EVP_DigestVerifyInit(vctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            EVP_DigestVerifyUpdate(vctx, signing_input.data(), signing_input.size()) == 1) {
            rc = EVP_DigestVerifyFinal(vctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size());
        }
        if (vctx) EVP_MD_CTX_free(vctx);
        CHECK(rc == 1);
        EVP_PKEY_free(pkey);
    }

    // Invalid PEM must throw.
    {
        bool t = false;
        try {
            ex::make_service_account_jwt("e", "not a pem", "s", "a", 1, 2);
        } catch (const std::exception&) { t = true; }
        CHECK(t);
    }

    if (g_failures == 0) {
        std::puts("gdrive_test passed");
        return 0;
    }
    std::fprintf(stderr, "gdrive_test FAILED (%d checks)\n", g_failures);
    return 1;
}
