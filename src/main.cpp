#include <Arduino.h>
#include "core/PortalGate.h"
#include "core/PortalDns.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_log.h>

#include "core/AuthManager.h"
#include "core/ClientTracker.h"
#include "core/ConfigStore.h"
#include "core/DhcpManager.h"
#include "core/NatManager.h"
#include "core/WifiManager.h"
#include "modules/LogBuffer.h"
#include "modules/SystemTools.h"
#include "version.h"
#include "web/WebApp.h"
#include "web/SecurePortal.h"

static const char* TAG = "main";

static ConfigStore configStore;
static AuthManager auth;
static WifiManager wifi;
static DhcpManager dhcp;
static NatManager nat;
static ClientTracker clients;
static SystemTools tools;
static WebApp web;

static PortalDns dnsServer;

void setup() {
    Serial.begin(115200);
    delay(200);

    LogBuffer::begin();
    LogBuffer::write('I', "%s v%s booting", ESP_ROUTER_NAME, ESP_ROUTER_VERSION);

    if (!configStore.begin()) {
        log_e("Config store init failed — running with defaults");
    }

    auth.begin(&configStore);

    wifi.onClientConnected = [](const String& mac) { clients.onConnected(mac); };
    wifi.onClientDisconnected = [](const String& mac) {
        PortalGate::disconnected(mac);
        clients.onDisconnected(mac);
    };
    wifi.onApApplied = []() { PortalGate::configure(configStore.cfg().dhcp.captivePortal); clients.watchDhcpNames(); dhcp.apply(); nat.reapply(); };
    // Re-advertise the upstream DNS whenever the WAN link changes. Applying
    // from the WiFi event task can block, so defer it to the main loop.
    wifi.onStaConnected = []() { dhcp.requestApply(); nat.reapply(); };
    wifi.onStaDisconnected = []() { dhcp.requestApply(); nat.reapply(); };

    clients.begin(&configStore);
    wifi.begin(&configStore);
    dhcp.begin(&configStore);
    nat.begin(&configStore);
    tools.begin(&configStore, &wifi);
    web.begin(&configStore, &auth, &wifi, &nat, &dhcp, &clients, &tools);

    const String hostname = configStore.cfg().system.hostname;
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        log_i("mDNS ready: http://%s.local", hostname.c_str());
    } else {
        log_w("mDNS start failed");
    }

    LogBuffer::write('I', "Panel ready at http://%s/", WiFi.softAPIP().toString().c_str());
    log_i("Setup required: %s", configStore.setupRequired() ? "yes" : "no");
}

void loop() {
    auth.loop();
    wifi.loop();
    dhcp.loop();
    nat.loop();
    clients.loop();
    tools.loop();
    web.loop();

    bool portal = configStore.setupRequired() || configStore.cfg().dhcp.captivePortal;
    dnsServer.loop(portal);
    static uint32_t tlsCheck = 0;
    if (!tlsCheck || millis() - tlsCheck >= 5000) {
        tlsCheck = millis();
        SecurePortal::update(configStore);
    }

    delay(2);
}
