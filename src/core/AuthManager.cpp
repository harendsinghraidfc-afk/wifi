#include "core/AuthManager.h"

#include <cstring>
#include <esp_log.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

static const char* TAG = "auth";
static const uint32_t PBKDF2_ITERATIONS = 12000;
static const uint32_t ATTEMPT_WINDOW_MS = 5UL * 60UL * 1000UL;
static const uint32_t LOCKOUT_MS = 60UL * 1000UL;

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hexToBytes(const String& hex, uint8_t* out, size_t outLen) {
    if (hex.length() != outLen * 2) {
        return false;
    }
    for (size_t i = 0; i < outLen; i++) {
        int hi = hexNibble(hex[i * 2]);
        int lo = hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool constantTimeEquals(const String& a, const String& b) {
    if (a.length() != b.length()) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

void AuthManager::begin(ConfigStore* store) {
    _store = store;
    if (setupRequired()) {
        setAdminPassword("admin");
    }
    log_i("Auth ready (setupRequired=%d)", setupRequired());
}

void AuthManager::loop() {
    uint32_t now = millis();
    if ((uint32_t)(now - _lastCleanup) < 60000UL) {
        return;
    }
    _lastCleanup = now;
    cleanup();
}

void AuthManager::cleanup() {
    uint32_t now = millis();

    for (auto& session : _sessions) {
        if (session.used && (uint32_t)(now - session.lastSeen) > SESSION_TIMEOUT_MS) {
            session.used = false;
            session.token = "";
            session.csrf = "";
        }
    }

    for (auto& attempt : _attempts) {
        if (!attempt.used) {
            continue;
        }
        bool windowExpired = (uint32_t)(now - attempt.windowStart) > ATTEMPT_WINDOW_MS;
        bool lockExpired = attempt.lockUntil == 0 || (int32_t)(attempt.lockUntil - now) <= 0;
        if (windowExpired && lockExpired) {
            attempt.used = false;
            attempt.ip = "";
            attempt.count = 0;
            attempt.lockUntil = 0;
        }
    }
}

bool AuthManager::setupRequired() const {
    return !_store || _store->setupRequired();
}

bool AuthManager::setAdminPassword(const String& password) {
    bool factoryDefault = setupRequired() && password == "admin";
    if (!factoryDefault && (password.length() < 8 || password.length() > 64)) {
        return false;
    }
    if (!_store) {
        return false;
    }

    String salt = randomHex(16);
    String hash = hashPassword(password, salt);
    if (salt.isEmpty() || hash.isEmpty()) {
        return false;
    }

    _store->secrets().adminSalt = salt;
    _store->secrets().adminHash = hash;
    _store->saveSecrets();
    log_i("Admin password updated");
    return true;
}

bool AuthManager::checkAdminPassword(const String& password) {
    if (setupRequired()) {
        return false;
    }
    return verifyPassword(password, _store->secrets().adminSalt, _store->secrets().adminHash);
}

bool AuthManager::changeAdminPassword(const String& current, const String& next) {
    if (!checkAdminPassword(current)) {
        return false;
    }
    return setAdminPassword(next);
}

String AuthManager::createSession(const String& ip) {
    cleanup();

    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!_sessions[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        // Evict the least recently used session.
        uint32_t oldest = UINT32_MAX;
        slot = 0;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (_sessions[i].lastSeen < oldest) {
                oldest = _sessions[i].lastSeen;
                slot = i;
            }
        }
    }

    Session& session = _sessions[slot];
    session.token = randomHex(32);
    session.csrf = randomHex(16);
    session.ip = ip;
    session.created = millis();
    session.lastSeen = session.created;
    session.used = true;

    log_i("Session created for %s", ip.c_str());
    return session.token;
}

bool AuthManager::validate(const String& token) {
    if (token.isEmpty()) {
        return false;
    }

    uint32_t now = millis();
    for (auto& session : _sessions) {
        if (!session.used || session.token != token) {
            continue;
        }
        if ((uint32_t)(now - session.lastSeen) > SESSION_TIMEOUT_MS) {
            session.used = false;
            return false;
        }
        session.lastSeen = now;
        return true;
    }
    return false;
}

String AuthManager::csrfFor(const String& token) {
    for (auto& session : _sessions) {
        if (session.used && session.token == token) {
            return session.csrf;
        }
    }
    return "";
}

void AuthManager::logout(const String& token) {
    for (auto& session : _sessions) {
        if (session.used && session.token == token) {
            session.used = false;
            session.token = "";
            session.csrf = "";
            log_i("Session closed for %s", session.ip.c_str());
            return;
        }
    }
}

bool AuthManager::isLocked(const String& ip, uint32_t& retryAfterSec) {
    uint32_t now = millis();
    for (auto& attempt : _attempts) {
        if (!attempt.used || attempt.ip != ip) {
            continue;
        }
        if (attempt.lockUntil != 0 && (int32_t)(attempt.lockUntil - now) > 0) {
            retryAfterSec = (attempt.lockUntil - now) / 1000UL + 1;
            return true;
        }
    }
    return false;
}

void AuthManager::noteFailure(const String& ip) {
    uint32_t now = millis();

    Attempt* slot = nullptr;
    for (auto& attempt : _attempts) {
        if (attempt.used && attempt.ip == ip) {
            slot = &attempt;
            break;
        }
    }
    if (!slot) {
        for (auto& attempt : _attempts) {
            if (!attempt.used) {
                slot = &attempt;
                slot->used = true;
                slot->ip = ip;
                slot->count = 0;
                slot->windowStart = now;
                slot->lockUntil = 0;
                break;
            }
        }
    }
    if (!slot) {
        slot = &_attempts[0];
        slot->used = true;
        slot->ip = ip;
        slot->count = 0;
        slot->windowStart = now;
        slot->lockUntil = 0;
    }

    if ((uint32_t)(now - slot->windowStart) > ATTEMPT_WINDOW_MS) {
        slot->count = 0;
        slot->windowStart = now;
    }

    slot->count++;
    log_w("Failed login from %s (%u/5)", ip.c_str(), (unsigned)slot->count);

    if (slot->count >= 5) {
        slot->lockUntil = now + LOCKOUT_MS;
        slot->count = 0;
        slot->windowStart = now;
        log_w("Login locked for %s", ip.c_str());
    }
}

void AuthManager::noteSuccess(const String& ip) {
    for (auto& attempt : _attempts) {
        if (attempt.used && attempt.ip == ip) {
            attempt.used = false;
            attempt.ip = "";
            attempt.count = 0;
            attempt.lockUntil = 0;
            return;
        }
    }
}

String AuthManager::hashPassword(const String& password, const String& saltHex) {
    uint8_t salt[16];
    if (!hexToBytes(saltHex, salt, sizeof(salt))) {
        return "";
    }

    uint8_t output[32];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                       (const unsigned char*)password.c_str(),
                                       password.length(),
                                       salt, sizeof(salt),
                                       PBKDF2_ITERATIONS,
                                       sizeof(output), output);

    if (rc != 0) {
        return "";
    }

    String out;
    out.reserve(sizeof(output) * 2);
    char buf[3];
    for (size_t i = 0; i < sizeof(output); i++) {
        snprintf(buf, sizeof(buf), "%02x", output[i]);
        out += buf;
    }
    return out;
}

bool AuthManager::verifyPassword(const String& password, const String& saltHex, const String& hashHex) {
    if (saltHex.isEmpty() || hashHex.isEmpty()) {
        return false;
    }
    String computed = hashPassword(password, saltHex);
    if (computed.isEmpty()) {
        return false;
    }
    return constantTimeEquals(computed, hashHex);
}

String AuthManager::cookieValue(const String& header, const char* name) {
    if (header.isEmpty() || name == nullptr) {
        return "";
    }

    int start = 0;
    while (start < (int)header.length()) {
        int end = header.indexOf(';', start);
        if (end < 0) {
            end = header.length();
        }

        String part = header.substring(start, end);
        part.trim();

        int eq = part.indexOf('=');
        if (eq > 0) {
            String key = part.substring(0, eq);
            key.trim();
            if (key == name) {
                String value = part.substring(eq + 1);
                value.trim();
                return value;
            }
        }

        start = end + 1;
    }
    return "";
}

String AuthManager::makeCookie(const String& token, bool clear) {
    String cookie = "SID=" + token + "; Path=/; HttpOnly; SameSite=Strict";
    if (clear) {
        cookie += "; Max-Age=0";
    }
    return cookie;
}

String AuthManager::randomHex(size_t bytes) {
    String out;
    out.reserve(bytes * 2);
    char buf[3];
    for (size_t i = 0; i < bytes; i++) {
        snprintf(buf, sizeof(buf), "%02x", (uint8_t)(esp_random() & 0xFF));
        out += buf;
    }
    return out;
}
