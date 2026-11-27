#include "core/ConfigStore.h"

#include <algorithm>
#include <cctype>
#include <esp_log.h>
#include <IPAddress.h>

static const char* TAG = "config";
static const char* CONFIG_PATH = "/config.json";
static const char* CONFIG_TMP = "/config.tmp";
static const char* NVS_NAMESPACE = "router";

bool ConfigStore::begin() {
    _mounted = LittleFS.begin(true);
    if (!_mounted) {
        log_e("LittleFS mount failed");
        return false;
    }

    _prefs.begin(NVS_NAMESPACE, false);
    loadSecrets();

    if (!loadConfig()) {
        log_w("No valid config found, using defaults");
        String err;
        validate(_cfg, err);
        save();
    }

    log_i("Config loaded (setupRequired=%d)", setupRequired());
    return true;
}

bool ConfigStore::loadConfig() {
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        log_e("Config parse error: %s", err.c_str());
        return false;
    }

    RouterConfig next;
    String error;
    if (!parseConfig(doc, next, error)) {
        log_e("Config validation error: %s", error.c_str());
        return false;
    }

    _cfg = next;
    return true;
}

bool ConfigStore::loadSecrets() {
    _secrets.apPassword = _prefs.getString("apPass", "");
    _secrets.wanPassword = _prefs.getString("wanPass", "");
    _secrets.adminHash = _prefs.getString("adminHash", "");
    _secrets.adminSalt = _prefs.getString("adminSalt", "");
    return true;
}

bool ConfigStore::apply(const RouterConfig& next, String& error) {
    std::lock_guard<std::mutex> lock(_configMutex);
    RouterConfig copy = next;
    if (!validate(copy, error)) {
        return false;
    }
    RouterConfig previous = _cfg;
    _cfg = copy;
    if (save()) return true;
    _cfg = previous;
    error = "Failed to persist settings";
    return false;
}

PortalConfig ConfigStore::portalSnapshot(bool& enabled) {
    std::lock_guard<std::mutex> lock(_configMutex);
    enabled = _cfg.dhcp.captivePortal;
    return _cfg.portal;
}

bool ConfigStore::save() {
    if (!_mounted) {
        return false;
    }

    String error;
    if (!validate(_cfg, error)) {
        log_e("Refusing to save invalid config: %s", error.c_str());
        return false;
    }

    JsonDocument doc;
    serializeConfig(doc);

    File f = LittleFS.open(CONFIG_TMP, "w");
    if (!f) {
        log_e("Failed to open temp config for write");
        return false;
    }
    bool complete = serializeJson(doc, f) == measureJson(doc) && !doc.overflowed();
    f.close();
    if (!complete || !LittleFS.rename(CONFIG_TMP, CONFIG_PATH)) {
        log_e("Failed to rename config file");
        return false;
    }

    log_i("Config saved");
    return true;
}

bool ConfigStore::saveSecrets() {
    _prefs.putString("apPass", _secrets.apPassword);
    _prefs.putString("wanPass", _secrets.wanPassword);
    _prefs.putString("adminHash", _secrets.adminHash);
    _prefs.putString("adminSalt", _secrets.adminSalt);
    return true;
}

void ConfigStore::serializeConfig(JsonDocument& doc) {
    doc["version"] = _cfg.version;

    JsonObject ap = doc["ap"].to<JsonObject>();
    ap["ssid"] = _cfg.ap.ssid;
    ap["ip"] = _cfg.ap.ip;
    ap["subnet"] = _cfg.ap.subnet;
    ap["channel"] = _cfg.ap.channel;
    ap["hidden"] = _cfg.ap.hidden;
    ap["maxClients"] = _cfg.ap.maxClients;
    ap["txPower"] = _cfg.ap.txPower;
    ap["band"] = _cfg.ap.band;

    JsonObject wan = doc["wan"].to<JsonObject>();
    wan["enabled"] = _cfg.wan.enabled;
    wan["ssid"] = _cfg.wan.ssid;
    wan["useStatic"] = _cfg.wan.useStatic;
    wan["ip"] = _cfg.wan.ip;
    wan["gateway"] = _cfg.wan.gateway;
    wan["subnet"] = _cfg.wan.subnet;
    wan["dns1"] = _cfg.wan.dns1;
    wan["dns2"] = _cfg.wan.dns2;
    wan["hostname"] = _cfg.wan.hostname;

    JsonObject dhcp = doc["dhcp"].to<JsonObject>();
    dhcp["captivePortal"] = _cfg.dhcp.captivePortal;
    dhcp["start"] = _cfg.dhcp.start;
    dhcp["end"] = _cfg.dhcp.end;
    dhcp["leaseMinutes"] = _cfg.dhcp.leaseMinutes;

    doc["nat"] = _cfg.natEnabled;
    auto p = doc["portal"].to<JsonObject>();
    p["title"] = _cfg.portal.title;
    p["message"] = _cfg.portal.message;
    p["mode"] = _cfg.portal.mode;
    p["minutes"] = _cfg.portal.minutes;
    p["webhook"] = _cfg.portal.webhook;
    p["code"] = _cfg.portal.code;
    p["secret"] = _cfg.portal.secret;
    p["apiKey"] = _cfg.portal.apiKey;

    JsonObject security = doc["security"].to<JsonObject>();
    security["wanAdmin"] = _cfg.security.wanAdmin;
    JsonArray blocked = security["blockedMacs"].to<JsonArray>();
    for (const auto& mac : _cfg.security.blockedMacs) {
        blocked.add(mac);
    }
    JsonArray names = security["names"].to<JsonArray>();
    for (const auto& item : _cfg.security.names) {
        JsonObject entry = names.add<JsonObject>();
        entry["mac"] = item.mac;
        entry["name"] = item.name;
    }

    JsonObject system = doc["system"].to<JsonObject>();
    system["hostname"] = _cfg.system.hostname;
    system["tzOffsetMinutes"] = _cfg.system.tzOffsetMinutes;
    system["ntpEnabled"] = _cfg.system.ntpEnabled;
    system["ntpServer"] = _cfg.system.ntpServer;
    system["scheduledReboot"] = _cfg.system.scheduledReboot;
    system["scheduledRebootTime"] = _cfg.system.scheduledRebootTime;
}

bool ConfigStore::parseConfig(JsonDocument& doc, RouterConfig& out, String& error) {
    if (!doc.is<JsonObject>()) {
        error = "Config must be a JSON object";
        return false;
    }

    JsonObject ap = doc["ap"].as<JsonObject>();
    if (!ap.isNull()) {
        out.ap.ssid = ap["ssid"] | out.ap.ssid;
        out.ap.ip = ap["ip"] | out.ap.ip;
        out.ap.subnet = ap["subnet"] | out.ap.subnet;
        out.ap.channel = ap["channel"] | out.ap.channel;
        out.ap.hidden = ap["hidden"] | out.ap.hidden;
        out.ap.maxClients = ap["maxClients"] | out.ap.maxClients;
        out.ap.txPower = ap["txPower"] | out.ap.txPower;
        out.ap.band = ap["band"] | out.ap.band;
    }

    JsonObject wan = doc["wan"].as<JsonObject>();
    if (!wan.isNull()) {
        out.wan.enabled = wan["enabled"] | out.wan.enabled;
        out.wan.ssid = wan["ssid"] | out.wan.ssid;
        out.wan.useStatic = wan["useStatic"] | out.wan.useStatic;
        out.wan.ip = wan["ip"] | out.wan.ip;
        out.wan.gateway = wan["gateway"] | out.wan.gateway;
        out.wan.subnet = wan["subnet"] | out.wan.subnet;
        out.wan.dns1 = wan["dns1"] | out.wan.dns1;
        out.wan.dns2 = wan["dns2"] | out.wan.dns2;
        out.wan.hostname = wan["hostname"] | out.wan.hostname;
    }

    JsonObject dhcp = doc["dhcp"].as<JsonObject>();
    if (!dhcp.isNull()) {
        out.dhcp.captivePortal = dhcp["captivePortal"] | false;
        out.dhcp.start = dhcp["start"] | out.dhcp.start;
        out.dhcp.end = dhcp["end"] | out.dhcp.end;
        out.dhcp.leaseMinutes = dhcp["leaseMinutes"] | out.dhcp.leaseMinutes;
    }

    out.natEnabled = doc["nat"] | out.natEnabled;
    auto p = doc["portal"];
    out.portal.title = p["title"] | out.portal.title;
    out.portal.message = p["message"] | out.portal.message;
    out.portal.mode = p["mode"] | out.portal.mode;
    out.portal.minutes = p["minutes"] | out.portal.minutes;
    out.portal.webhook = p["webhook"] | "";
    out.portal.code = p["code"] | "";
    out.portal.secret = p["secret"] | "";
    out.portal.apiKey = p["apiKey"] | "";

    JsonObject security = doc["security"].as<JsonObject>();
    if (!security.isNull()) {
        out.security.wanAdmin = security["wanAdmin"] | out.security.wanAdmin;

        out.security.blockedMacs.clear();
        for (JsonVariant v : security["blockedMacs"].as<JsonArray>()) {
            if (v.is<const char*>()) {
                out.security.blockedMacs.push_back(String(v.as<const char*>()));
            }
        }

        out.security.names.clear();
        for (JsonObject entry : security["names"].as<JsonArray>()) {
            ClientName item;
            item.mac = entry["mac"] | "";
            item.name = entry["name"] | "";
            if (!item.mac.isEmpty()) {
                out.security.names.push_back(item);
            }
        }
    }

    JsonObject system = doc["system"].as<JsonObject>();
    if (!system.isNull()) {
        out.system.hostname = system["hostname"] | out.system.hostname;
        out.system.tzOffsetMinutes = system["tzOffsetMinutes"] | out.system.tzOffsetMinutes;
        out.system.ntpEnabled = system["ntpEnabled"] | out.system.ntpEnabled;
        out.system.ntpServer = system["ntpServer"] | out.system.ntpServer;
        out.system.scheduledReboot = system["scheduledReboot"] | out.system.scheduledReboot;
        out.system.scheduledRebootTime = system["scheduledRebootTime"] | out.system.scheduledRebootTime;
    }

    return validate(out, error);
}

bool ConfigStore::validate(RouterConfig& cfg, String& error) {
    const auto& p = cfg.portal;
    if ((p.mode != "click" && p.mode != "code" && p.mode != "webhook") ||
        p.minutes < 1 || p.minutes > 10080 || p.title.length() > 96 ||
        p.message.length() > 512 || p.code.length() > 64 || p.secret.length() > 128 ||
        p.webhook.length() > 512 || p.apiKey.length() > 128 ||
        (p.mode == "code" && p.code.isEmpty()) ||
        (p.mode == "webhook" && !p.webhook.startsWith("http://"))) {
        error = "Invalid portal settings (webhook requires a local HTTP endpoint)";
        return false;
    }
    cfg.ap.ssid.trim();
    if (cfg.ap.ssid.isEmpty() || cfg.ap.ssid.length() > 32) {
        error = "AP SSID must be 1-32 characters";
        return false;
    }

    if (cfg.ap.band != "2.4" && cfg.ap.band != "5" && cfg.ap.band != "auto") {
        cfg.ap.band = "2.4";
    }
    bool is5gChannel = cfg.ap.channel >= 36;
    if (cfg.ap.channel < 1 || cfg.ap.channel > 165 || (cfg.ap.channel > 14 && !is5gChannel)) {
        cfg.ap.channel = 6;
    }
    if (cfg.ap.band == "2.4" && is5gChannel) {
        cfg.ap.channel = 6;
    } else if (cfg.ap.band == "5" && !is5gChannel) {
        cfg.ap.channel = 36;
    }
    cfg.ap.maxClients = constrain(cfg.ap.maxClients, 1, 10);
    cfg.ap.txPower = constrain(cfg.ap.txPower, 2, 20);

    IPAddress apIp, apMask;
    if (!apIp.fromString(cfg.ap.ip) || !apMask.fromString(cfg.ap.subnet)) {
        error = "Invalid AP IP or subnet";
        return false;
    }

    IPAddress poolStart, poolEnd;
    if (!poolStart.fromString(cfg.dhcp.start) || !poolEnd.fromString(cfg.dhcp.end)) {
        error = "Invalid DHCP pool start/end";
        return false;
    }
    uint32_t mask = (uint32_t)apMask;
    uint32_t apAddr = (uint32_t)apIp;
    uint32_t startAddr = (uint32_t)poolStart;
    uint32_t endAddr = (uint32_t)poolEnd;
    if ((startAddr & mask) != (apAddr & mask) || (endAddr & mask) != (apAddr & mask)) {
        error = "DHCP pool must be inside the AP subnet";
        return false;
    }
    if (startAddr == apAddr || endAddr == apAddr) {
        error = "DHCP pool cannot include the router IP";
        return false;
    }
    if (startAddr > endAddr) {
        error = "DHCP pool start must be lower than end";
        return false;
    }
    cfg.dhcp.leaseMinutes = constrain(cfg.dhcp.leaseMinutes, 1, 10080);

    cfg.wan.ssid.trim();
    if (cfg.wan.ssid.length() > 32) {
        error = "WAN SSID must be at most 32 characters";
        return false;
    }
    if (cfg.wan.useStatic && cfg.wan.enabled) {
        IPAddress wanIp, wanGw, wanMask;
        if (!wanIp.fromString(cfg.wan.ip) || !wanGw.fromString(cfg.wan.gateway) ||
            !wanMask.fromString(cfg.wan.subnet)) {
            error = "Invalid static WAN IP configuration";
            return false;
        }
    }

    cfg.system.hostname.trim();
    if (cfg.system.hostname.isEmpty() || cfg.system.hostname.length() > 32) {
        error = "Hostname must be 1-32 characters";
        return false;
    }
    for (size_t i = 0; i < cfg.system.hostname.length(); i++) {
        char c = cfg.system.hostname[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
            error = "Hostname may only contain letters, digits, '-' and '_'";
            return false;
        }
    }
    cfg.system.tzOffsetMinutes = constrain(cfg.system.tzOffsetMinutes, -720, 840);
    cfg.system.ntpServer.trim();
    if (cfg.system.ntpServer.isEmpty()) {
        cfg.system.ntpServer = "pool.ntp.org";
    }
    if (cfg.system.scheduledRebootTime.length() != 5 ||
        cfg.system.scheduledRebootTime[2] != ':') {
        cfg.system.scheduledRebootTime = "03:00";
    }

    // Normalise and dedupe MAC block list.
    std::vector<String> blocked;
    for (const auto& raw : cfg.security.blockedMacs) {
        String mac = normalizeMac(raw);
        if (mac.length() != 17) {
            continue;
        }
        if (std::find(blocked.begin(), blocked.end(), mac) == blocked.end()) {
            blocked.push_back(mac);
        }
    }
    cfg.security.blockedMacs = blocked;

    // Normalise client names.
    std::vector<ClientName> names;
    for (auto& item : cfg.security.names) {
        String mac = normalizeMac(item.mac);
        item.name.trim();
        if (mac.length() != 17 || item.name.isEmpty() || item.name.length() > 32) {
            continue;
        }
        bool duplicate = false;
        for (auto& existing : names) {
            if (existing.mac == mac) {
                existing.name = item.name;
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            names.push_back({mac, item.name});
        }
    }
    cfg.security.names = names;

    return true;
}

String ConfigStore::backupJson(bool includeSecrets) {
    JsonDocument doc;
    serializeConfig(doc);

    if (includeSecrets) {
        JsonObject secrets = doc["secrets"].to<JsonObject>();
        secrets["apPassword"] = _secrets.apPassword;
        secrets["wanPassword"] = _secrets.wanPassword;
        secrets["adminHash"] = _secrets.adminHash;
        secrets["adminSalt"] = _secrets.adminSalt;
    } else {
        for (const char* key : {"code", "secret", "apiKey"}) doc["portal"].remove(key);
    }

    String out;
    serializeJsonPretty(doc, out);
    return out;
}

bool ConfigStore::restoreJson(const String& json, String& error) {
    std::lock_guard<std::mutex> lock(_configMutex);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        error = String("Invalid JSON: ") + err.c_str();
        return false;
    }

    RouterConfig next;
    for (const char* key : {"code", "secret", "apiKey"}) {
        if (doc["portal"].is<JsonObject>() && doc["portal"][key].isNull()) {
            doc["portal"][key] = !strcmp(key, "code") ? _cfg.portal.code :
                (!strcmp(key, "secret") ? _cfg.portal.secret : _cfg.portal.apiKey);
        }
    }
    if (!parseConfig(doc, next, error)) {
        return false;
    }

    JsonObject secrets = doc["secrets"].as<JsonObject>();
    if (!secrets.isNull()) {
        _secrets.apPassword = secrets["apPassword"] | _secrets.apPassword;
        _secrets.wanPassword = secrets["wanPassword"] | _secrets.wanPassword;
        _secrets.adminHash = secrets["adminHash"] | _secrets.adminHash;
        _secrets.adminSalt = secrets["adminSalt"] | _secrets.adminSalt;
        saveSecrets();
    }

    _cfg = next;
    save();
    return true;
}

void ConfigStore::factoryReset() {
    log_w("Factory reset requested");
    _prefs.clear();
    LittleFS.remove(CONFIG_PATH);
    LittleFS.remove(CONFIG_TMP);
    LittleFS.remove("/portal.html");
    LittleFS.remove("/portal.tmp");
    LittleFS.remove("/tls-identity");
    LittleFS.remove("/tls-identity.tmp");
    delay(200);
    ESP.restart();
}

String ConfigStore::clientName(const String& mac) const {
    String normalized = normalizeMac(mac);
    for (const auto& item : _cfg.security.names) {
        if (item.mac == normalized) {
            return item.name;
        }
    }
    return "";
}

void ConfigStore::setClientName(const String& mac, const String& name) {
    String normalized = normalizeMac(mac);
    if (normalized.length() != 17) {
        return;
    }

    String clean = name;
    clean.trim();
    if (clean.length() > 32) {
        clean = clean.substring(0, 32);
    }

    for (auto it = _cfg.security.names.begin(); it != _cfg.security.names.end(); ++it) {
        if (it->mac == normalized) {
            if (clean.isEmpty()) {
                _cfg.security.names.erase(it);
            } else {
                it->name = clean;
            }
            save();
            return;
        }
    }

    if (!clean.isEmpty()) {
        _cfg.security.names.push_back({normalized, clean});
        save();
    }
}

bool ConfigStore::isBlocked(const String& mac) const {
    String normalized = normalizeMac(mac);
    for (const auto& blocked : _cfg.security.blockedMacs) {
        if (blocked == normalized) {
            return true;
        }
    }
    return false;
}

bool ConfigStore::setBlocked(const String& mac, bool blocked) {
    String normalized = normalizeMac(mac);
    if (normalized.length() != 17) {
        return false;
    }

    auto it = std::find(_cfg.security.blockedMacs.begin(), _cfg.security.blockedMacs.end(), normalized);
    if (blocked && it == _cfg.security.blockedMacs.end()) {
        _cfg.security.blockedMacs.push_back(normalized);
        save();
    } else if (!blocked && it != _cfg.security.blockedMacs.end()) {
        _cfg.security.blockedMacs.erase(it);
        save();
    }
    return true;
}

String ConfigStore::normalizeMac(const String& mac) {
    String hex;
    hex.reserve(12);
    for (size_t i = 0; i < mac.length(); i++) {
        char c = mac[i];
        if (isxdigit((unsigned char)c)) {
            hex += (char)tolower((unsigned char)c);
        }
    }
    if (hex.length() != 12) {
        return "";
    }

    String out;
    out.reserve(17);
    for (int i = 0; i < 12; i += 2) {
        if (i > 0) {
            out += ':';
        }
        out += hex.substring(i, i + 2);
    }
    return out;
}
