#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include "core/ConfigStore.h"

class WifiManager {
public:
    void begin(ConfigStore* store);
    void loop();

    bool connectWan(String& error);
    void disconnectWan();
    void scheduleApRestart(uint32_t delayMs = 800);
    void applyApNow();

    String scanJson();
    String statusJson();

    bool staConnected() const;
    bool staConfigured() const;
    String staIp() const;
    int staRssi() const;
    String staSsid() const;
    int apClientCount() const;
    String apIp() const;
    int effectiveChannel() const;
    bool bandSupported() const;

    bool deauthMac(const String& mac);

    std::function<void(const String&)> onClientConnected;
    std::function<void(const String&)> onClientDisconnected;
    std::function<void()> onStaConnected;
    std::function<void()> onStaDisconnected;
    std::function<void()> onApApplied;

private:
    ConfigStore* _store = nullptr;
    bool _apStarted = false;
    uint32_t _lastReconnect = 0;
    uint32_t _pendingApRestart = 0;
    bool _staWasConnected = false;

    void startAp();
    void startSta();
    void registerEvents();
    void applyBandMode();

    static String macToString(const uint8_t* mac);
    static bool parseMac(const String& text, uint8_t* mac);
    static bool parseIp(const String& text, IPAddress& out);
    static String authName(wifi_auth_mode_t mode);
};
