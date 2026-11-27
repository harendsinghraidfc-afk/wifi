#include "core/WifiManager.h"
#include "modules/LogBuffer.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <sdkconfig.h>

static const char* TAG = "wifi";

void WifiManager::begin(ConfigStore* store) {
    _store = store;

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);

    registerEvents();
    startAp();

    const auto& wan = _store->cfg().wan;
    if (wan.enabled && !wan.ssid.isEmpty() && !_store->setupRequired()) {
        startSta();
    }

    log_i("WiFi started (AP=%s, setup=%d)", _apStarted ? "up" : "failed", _store->setupRequired());
}

void WifiManager::loop() {
    if (!_store) {
        return;
    }

    uint32_t now = millis();

    if (_pendingApRestart != 0 && (int32_t)(now - _pendingApRestart) >= 0) {
        _pendingApRestart = 0;
        applyApNow();
    }

    const auto& wan = _store->cfg().wan;
    if (wan.enabled && !wan.ssid.isEmpty() && !_store->setupRequired()) {
        if (WiFi.status() != WL_CONNECTED && (uint32_t)(now - _lastReconnect) > 15000UL) {
            log_i("WAN reconnect attempt");
            WiFi.reconnect();
            _lastReconnect = now;
        }
    }
}

void WifiManager::registerEvents() {
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
                String mac = macToString(info.wifi_ap_staconnected.mac);
                LogBuffer::write('I', "AP client connected: %s", mac.c_str());
                if (onClientConnected) {
                    onClientConnected(mac);
                }
                break;
            }
            case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
                String mac = macToString(info.wifi_ap_stadisconnected.mac);
                LogBuffer::write('I', "AP client disconnected: %s", mac.c_str());
                if (onClientDisconnected) {
                    onClientDisconnected(mac);
                }
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
                _staWasConnected = true;
                _lastReconnect = millis();
                LogBuffer::write('I', "WAN connected: %s", WiFi.localIP().toString().c_str());
                if (onStaConnected) {
                    onStaConnected();
                }
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                if (_staWasConnected) {
                    _staWasConnected = false;
                    LogBuffer::write('W', "WAN disconnected (reason=%d)", info.wifi_sta_disconnected.reason);
                    if (onStaDisconnected) {
                        onStaDisconnected();
                    }
                }
                break;
            }
            default:
                break;
        }
    });
}

void WifiManager::applyBandMode() {
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G) && CONFIG_SOC_WIFI_SUPPORT_5G
    wifi_band_mode_t mode = WIFI_BAND_MODE_2G_ONLY;
    if (_store->cfg().ap.band == "5") {
        mode = WIFI_BAND_MODE_5G_ONLY;
    } else if (_store->cfg().ap.band == "auto") {
        mode = WIFI_BAND_MODE_AUTO;
    }

    esp_err_t err = esp_wifi_set_band_mode(mode);
    if (err != ESP_OK) {
        log_w("Failed to set WiFi band mode: %s", esp_err_to_name(err));
    } else {
        log_i("WiFi band mode set to %d", (int)mode);
    }
#endif
}

void WifiManager::startAp() {
    const RouterConfig& cfg = _store->cfg();
    bool setup = _store->setupRequired();

    String ssid = setup ? "ESP-Router-Setup" : cfg.ap.ssid;
    String password = setup ? "" : _store->secrets().apPassword;

    bool useSta = cfg.wan.enabled && !cfg.wan.ssid.isEmpty() && !setup;
    wifi_mode_t desired = useSta ? WIFI_AP_STA : WIFI_AP;
    if (WiFi.getMode() != desired) {
        WiFi.mode(desired);
    }
    applyBandMode();

    IPAddress ip, mask;
    if (!parseIp(cfg.ap.ip, ip)) {
        ip = IPAddress(192, 168, 4, 1);
    }
    if (!parseIp(cfg.ap.subnet, mask)) {
        mask = IPAddress(255, 255, 255, 0);
    }

    WiFi.softAPConfig(ip, ip, mask);
    WiFi.softAPsetHostname(cfg.system.hostname.c_str());

    int channel = cfg.ap.channel;
    bool ok = WiFi.softAP(ssid.c_str(),
                          password.isEmpty() ? nullptr : password.c_str(),
                          channel,
                          cfg.ap.hidden ? 1 : 0,
                          cfg.ap.maxClients);

    if (!ok && channel != 1) {
        log_w("softAP failed on channel %d, retrying on channel 1", channel);
        ok = WiFi.softAP(ssid.c_str(),
                         password.isEmpty() ? nullptr : password.c_str(),
                         1,
                         cfg.ap.hidden ? 1 : 0,
                         cfg.ap.maxClients);
    }

    WiFi.softAPbandwidth(WIFI_BW_HT20);

    int qdbm = cfg.ap.txPower * 4;
    qdbm = constrain(qdbm, 8, 78);
    WiFi.setTxPower((wifi_power_t)qdbm);

    _apStarted = ok;
    if (ok) {
        log_i("AP '%s' started on channel %d (ip=%s)", ssid.c_str(), effectiveChannel(), apIp().c_str());
    } else {
        log_e("Failed to start AP '%s'", ssid.c_str());
    }

    if (onApApplied) {
        onApApplied();
    }
}

void WifiManager::startSta() {
    const WanConfig& wan = _store->cfg().wan;
    if (!wan.enabled || wan.ssid.isEmpty()) {
        return;
    }

    if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
    }

    WiFi.setHostname(_store->cfg().system.hostname.c_str());

    if (wan.useStatic) {
        IPAddress ip, gateway, subnet, dns1, dns2;
        if (parseIp(wan.ip, ip) && parseIp(wan.gateway, gateway)) {
            if (!parseIp(wan.subnet, subnet)) {
                subnet = IPAddress(255, 255, 255, 0);
            }
            if (!parseIp(wan.dns1, dns1)) {
                dns1 = gateway;
            }
            if (!parseIp(wan.dns2, dns2)) {
                dns2 = dns1;
            }
            WiFi.config(ip, gateway, subnet, dns1, dns2);
            log_i("WAN static config: %s/%s gw=%s", wan.ip.c_str(), wan.subnet.c_str(), wan.gateway.c_str());
        }
    }

    const String& password = _store->secrets().wanPassword;
    WiFi.begin(wan.ssid.c_str(), password.isEmpty() ? nullptr : password.c_str());
    _lastReconnect = millis();
    log_i("Connecting to WAN '%s'", wan.ssid.c_str());
}

bool WifiManager::connectWan(String& error) {
    if (!_store) {
        error = "Config not ready";
        return false;
    }
    if (_store->cfg().wan.ssid.isEmpty()) {
        error = "Upstream SSID is required";
        return false;
    }

    WiFi.disconnect(false);
    delay(100);
    startSta();
    return true;
}

void WifiManager::disconnectWan() {
    WiFi.disconnect(true);
    _staWasConnected = false;
    log_i("WAN disconnected by user");
}

void WifiManager::scheduleApRestart(uint32_t delayMs) {
    _pendingApRestart = millis() + (delayMs == 0 ? 1 : delayMs);
}

void WifiManager::applyApNow() {
    log_i("Restarting AP with new settings");
    WiFi.softAPdisconnect(true);
    delay(100);
    startAp();
}

String WifiManager::scanJson() {
    JsonDocument doc;
    int count = WiFi.scanNetworks(false, true);

    if (count < 0) {
        doc["error"] = "WiFi scan failed";
        String out;
        serializeJson(doc, out);
        return out;
    }

    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject entry = networks.add<JsonObject>();
        entry["ssid"] = WiFi.SSID(i);
        entry["rssi"] = WiFi.RSSI(i);
        entry["channel"] = WiFi.channel(i);
        entry["bssid"] = WiFi.BSSIDstr(i);
        entry["auth"] = authName(WiFi.encryptionType(i));
        entry["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        entry["band"] = WiFi.channel(i) > 14 ? "5" : "2.4";
    }
    WiFi.scanDelete();

    String out;
    serializeJson(doc, out);
    return out;
}

String WifiManager::statusJson() {
    JsonDocument doc;

    JsonObject ap = doc["ap"].to<JsonObject>();
    ap["ssid"] = _store->setupRequired() ? "ESP-Router-Setup" : _store->cfg().ap.ssid;
    ap["ip"] = apIp();
    ap["subnet"] = _store->cfg().ap.subnet;
    ap["clients"] = apClientCount();
    ap["channel"] = effectiveChannel();
    ap["hidden"] = _store->cfg().ap.hidden;
    ap["band"] = _store->cfg().ap.band;
    ap["maxClients"] = _store->cfg().ap.maxClients;
    ap["txPower"] = _store->cfg().ap.txPower;
    ap["passwordSet"] = !_store->secrets().apPassword.isEmpty();
    ap["running"] = _apStarted;
    ap["setupMode"] = _store->setupRequired();

    JsonObject sta = doc["sta"].to<JsonObject>();
    bool configured = _store->cfg().wan.enabled && !_store->cfg().wan.ssid.isEmpty();
    bool connected = WiFi.status() == WL_CONNECTED;
    sta["configured"] = configured;
    sta["connected"] = connected;
    sta["ssid"] = _store->cfg().wan.ssid;
    sta["useStatic"] = _store->cfg().wan.useStatic;
    sta["passwordSet"] = !_store->secrets().wanPassword.isEmpty();

    if (connected) {
        sta["ip"] = WiFi.localIP().toString();
        sta["gateway"] = WiFi.gatewayIP().toString();
        sta["rssi"] = WiFi.RSSI();
        sta["channel"] = WiFi.channel();
        sta["bssid"] = WiFi.BSSIDstr();
        sta["dns"] = WiFi.dnsIP().toString();
    }

    doc["hostname"] = _store->cfg().system.hostname;
    doc["bandSupported"] = bandSupported();
    doc["setupRequired"] = _store->setupRequired();

    String out;
    serializeJson(doc, out);
    return out;
}

bool WifiManager::staConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::staConfigured() const {
    return _store && _store->cfg().wan.enabled && !_store->cfg().wan.ssid.isEmpty();
}

String WifiManager::staIp() const {
    return WiFi.localIP().toString();
}

int WifiManager::staRssi() const {
    return WiFi.RSSI();
}

String WifiManager::staSsid() const {
    return WiFi.SSID();
}

int WifiManager::apClientCount() const {
    return WiFi.softAPgetStationNum();
}

String WifiManager::apIp() const {
    return WiFi.softAPIP().toString();
}

int WifiManager::effectiveChannel() const {
    return WiFi.channel();
}

bool WifiManager::bandSupported() const {
#if defined(CONFIG_SOC_WIFI_SUPPORT_5G) && CONFIG_SOC_WIFI_SUPPORT_5G
    return true;
#else
    return false;
#endif
}

bool WifiManager::deauthMac(const String& macText) {
    uint8_t mac[6];
    if (!parseMac(macText, mac)) {
        return false;
    }

    uint16_t aid = 0;
    if (esp_wifi_ap_get_sta_aid(mac, &aid) != ESP_OK) {
        return false;
    }
    return esp_wifi_deauth_sta(aid) == ESP_OK;
}

String WifiManager::macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

bool WifiManager::parseMac(const String& text, uint8_t* mac) {
    String normalized = ConfigStore::normalizeMac(text);
    if (normalized.length() != 17) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)strtoul(normalized.substring(i * 3, i * 3 + 2).c_str(), nullptr, 16);
    }
    return true;
}

bool WifiManager::parseIp(const String& text, IPAddress& out) {
    if (text.isEmpty()) {
        return false;
    }
    return out.fromString(text);
}

String WifiManager::authName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "Secured";
    }
}
