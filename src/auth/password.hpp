// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          src/auth/password.hpp
// Purpose:       Password hashing (PBKDF2-HMAC-SHA256) and secure random tokens
//                for API/web-UI authentication.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace nightwatcher::auth {

// Hash a password with a fresh random salt. Returns an encoded string of the
// form "pbkdf2_sha256$<iterations>$<salt_hex>$<hash_hex>".
std::string hash_password(const std::string& password);

// Verify a password against a string produced by hash_password(). Constant-time
// comparison; returns false on any parse error.
bool verify_password(const std::string& password, const std::string& encoded);

// Cryptographically-random lowercase hex string of `nbytes` bytes (2*nbytes chars).
std::string random_hex(int nbytes);

}  // namespace nightwatcher::auth
