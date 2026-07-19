// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/tls_cert_test.cpp
// Purpose:       Unit test for ensure_self_signed_cert: it generates a readable
//                cert/key pair with the requested CN, is idempotent when the
//                files already exist, and creates missing parent directories.
// Created:       2026-07-19
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "tls_cert.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond);       \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

int main() {
    const std::string dir = "nw_tls_test_dir";  // exercises parent-dir creation
    const std::string cert = dir + "/cert.pem";
    const std::string key = dir + "/key.pem";
    ::unlink(cert.c_str());
    ::unlink(key.c_str());
    ::rmdir(dir.c_str());

    // First call generates the pair.
    std::string info;
    CHECK(nightwatcher::ensure_self_signed_cert(cert, key, "nwhost", info));
    CHECK(info.find("generated") != std::string::npos);
    CHECK(std::ifstream(cert).good());
    CHECK(std::ifstream(key).good());

    // The certificate parses and carries the requested common name.
    if (FILE* f = std::fopen(cert.c_str(), "r")) {
        X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
        std::fclose(f);
        CHECK(x != nullptr);
        if (x) {
            char cn[128] = {0};
            X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, cn, sizeof(cn));
            CHECK(std::string(cn) == "nwhost");
            // SANs must cover loopback + the CN so browsers validate those hosts.
            CHECK(X509_check_ip_asc(x, "127.0.0.1", 0) == 1);
            CHECK(X509_check_host(x, "localhost", 0, 0, nullptr) == 1);
            CHECK(X509_check_host(x, "nwhost", 0, 0, nullptr) == 1);
            X509_free(x);
        }
    } else {
        CHECK(false);  // cert unreadable
    }

    // The private key parses.
    if (FILE* f = std::fopen(key.c_str(), "r")) {
        EVP_PKEY* pk = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
        std::fclose(f);
        CHECK(pk != nullptr);
        if (pk) EVP_PKEY_free(pk);
    } else {
        CHECK(false);  // key unreadable
    }

    // Second call is idempotent: it keeps the existing files untouched.
    std::string info2;
    CHECK(nightwatcher::ensure_self_signed_cert(cert, key, "ignored-cn", info2));
    CHECK(info2.find("existing") != std::string::npos);

    ::unlink(cert.c_str());
    ::unlink(key.c_str());
    ::rmdir(dir.c_str());

    if (g_failures == 0) {
        std::puts("tls_cert_test passed");
        return 0;
    }
    std::fprintf(stderr, "tls_cert_test FAILED (%d checks)\n", g_failures);
    return 1;
}
