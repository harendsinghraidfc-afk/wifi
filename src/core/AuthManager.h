#pragma once

#include <Arduino.h>
#include "core/ConfigStore.h"

class AuthManager {
public:
    void begin(ConfigStore* store);
    void loop();

    bool setupRequired() const;
    bool setAdminPassword(const String& password);
    bool checkAdminPassword(const String& password);
    bool changeAdminPassword(const String& current, const String& next);

    String createSession(const String& ip);
    bool validate(const String& token);
    String csrfFor(const String& token);
    void logout(const String& token);

    bool isLocked(const String& ip, uint32_t& retryAfterSec);
    void noteFailure(const String& ip);
    void noteSuccess(const String& ip);

    static String cookieValue(const String& header, const char* name);
    static String makeCookie(const String& token, bool clear = false);
    static String randomHex(size_t bytes);

private:
    static constexpr uint32_t SESSION_TIMEOUT_MS = 30UL * 60UL * 1000UL;
    static const int MAX_SESSIONS = 4;
    static const int MAX_ATTEMPTS = 8;

    struct Session {
        String token;
        String csrf;
        String ip;
        uint32_t created = 0;
        uint32_t lastSeen = 0;
        bool used = false;
    };

    struct Attempt {
        String ip;
        uint8_t count = 0;
        uint32_t windowStart = 0;
        uint32_t lockUntil = 0;
        bool used = false;
    };

    ConfigStore* _store = nullptr;
    Session _sessions[MAX_SESSIONS];
    Attempt _attempts[MAX_ATTEMPTS];
    uint32_t _lastCleanup = 0;

    String hashPassword(const String& password, const String& saltHex);
    bool verifyPassword(const String& password, const String& saltHex, const String& hashHex);
    void cleanup();
};
