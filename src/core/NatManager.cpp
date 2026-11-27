#include "core/NatManager.h"
#include "core/PortalGate.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>

static const char* TAG = "nat";

void NatManager::begin(ConfigStore* store) {
    _store = store;
    _lastError = "";
    _lastCheck = 0;
}

void NatManager::loop() {
    if (!_store) {
        return;
    }

    uint32_t now = millis();
    if ((uint32_t)(now - _lastCheck) < 1000UL) {
        return;
    }
    _lastCheck = now;

    bool desired = _store->cfg().natEnabled && WiFi.status() == WL_CONNECTED && !_store->setupRequired();
    if (_store->cfg().dhcp.captivePortal && !PortalGate::ready()) desired = false;

    if (desired && !_active) {
        enable();
    } else if (!desired && _active) {
        disable();
    }
}

bool NatManager::enable() {
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) {
        _lastError = "AP interface not ready";
        return false;
    }

    esp_err_t err = esp_netif_napt_enable(ap);
    if (err == ESP_OK) {
        _active = true;
        _supported = true;
        _lastError = "";
        log_i("NAPT enabled on AP interface");
        return true;
    }

    _lastError = esp_err_to_name(err);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        _supported = false;
        log_e("NAPT not supported by this firmware build");
    } else {
        log_w("Failed to enable NAPT: %s", _lastError.c_str());
    }
    return false;
}

void NatManager::disable() {
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_napt_disable(ap);
    }
    _active = false;
    log_i("NAPT disabled");
}

String NatManager::statusJson() {
    JsonDocument doc;
    doc["enabled"] = _store ? _store->cfg().natEnabled : false;
    doc["active"] = _active;
    doc["supported"] = _supported;
    doc["upstreamConnected"] = WiFi.status() == WL_CONNECTED;
    doc["error"] = _lastError;

    String out;
    serializeJson(doc, out);
    return out;
}
