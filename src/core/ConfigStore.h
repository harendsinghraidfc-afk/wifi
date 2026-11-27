#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <mutex>

struct ApConfig {
    String ssid = "Lesh";
    String ip = "192.168.4.1";
    String subnet = "255.255.255.0";
    int channel = 6;
    bool hidden = false;
    int maxClients = 8;
    int txPower = 20;
    String band = "2.4"; // 2.4 | 5 | auto (5/auto only on 5GHz-capable targets)
};

struct WanConfig {
    bool enabled = false;
    String ssid;
    bool useStatic = false;
    String ip;
    String gateway;
    String subnet = "255.255.255.0";
    String dns1;
    String dns2;
    String hostname = "esp-router";
};

struct DhcpConfig {
    bool captivePortal = false;
    String start = "192.168.4.10";
    String end = "192.168.4.100";
    int leaseMinutes = 120;
};

struct ClientName {
    String mac;
    String name;
};

struct SecurityConfig {
    bool wanAdmin = false;
    std::vector<String> blockedMacs;
    std::vector<ClientName> names;
};

struct SystemConfig {
    String hostname = "esp-router";
    int tzOffsetMinutes = 330; // IST
    bool ntpEnabled = true;
    String ntpServer = "pool.ntp.org";
    bool scheduledReboot = false;
    String scheduledRebootTime = "03:00";
};

struct PortalConfig {
        String title = "Welcome to Lesh WiFi";
        String message = "Connect this device to the internet.";
        String mode = "click";
        String code, webhook, secret, apiKey;
        int minutes = 60;
};

struct RouterConfig {
    PortalConfig portal;
    int version = 1;
    ApConfig ap;
    WanConfig wan;
    DhcpConfig dhcp;
    bool natEnabled = true;
    SecurityConfig security;
    SystemConfig system;
};

struct Secrets {
    String apPassword;
    String wanPassword;
    String adminHash;
    String adminSalt;
};

class ConfigStore {
public:
    bool begin();
    PortalConfig portalSnapshot(bool& enabled);

    RouterConfig& cfg() { return _cfg; }
    const RouterConfig& cfg() const { return _cfg; }
    Secrets& secrets() { return _secrets; }

    bool save();
    bool saveSecrets();
    bool apply(const RouterConfig& next, String& error);
    bool setupRequired() const { return _secrets.adminHash.isEmpty(); }

    String backupJson(bool includeSecrets);
    bool restoreJson(const String& json, String& error);
    void factoryReset();

    String clientName(const String& mac) const;
    void setClientName(const String& mac, const String& name);
    bool isBlocked(const String& mac) const;
    bool setBlocked(const String& mac, bool blocked);

    static String normalizeMac(const String& mac);

private:
    std::mutex _configMutex;
    RouterConfig _cfg;
    Secrets _secrets;
    Preferences _prefs;
    bool _mounted = false;

    bool loadConfig();
    bool loadSecrets();
    bool parseConfig(JsonDocument& doc, RouterConfig& out, String& error);
    bool validate(RouterConfig& cfg, String& error);
    void serializeConfig(JsonDocument& doc);
};
