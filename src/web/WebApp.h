#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <functional>

#include "core/AuthManager.h"
#include "core/ClientTracker.h"
#include "core/ConfigStore.h"
#include "core/DhcpManager.h"
#include "core/NatManager.h"
#include "core/WifiManager.h"
#include "modules/SystemTools.h"

class WebApp {
public:
    void begin(ConfigStore* store,
               AuthManager* auth,
               WifiManager* wifi,
               NatManager* nat,
               DhcpManager* dhcp,
               ClientTracker* clients,
               SystemTools* tools);
    void loop();

private:
    AsyncWebServer* _server = nullptr;
    AsyncWebSocket* _ws = nullptr;

    ConfigStore* _store = nullptr;
    AuthManager* _auth = nullptr;
    WifiManager* _wifi = nullptr;
    NatManager* _nat = nullptr;
    DhcpManager* _dhcp = nullptr;
    ClientTracker* _clients = nullptr;
    SystemTools* _tools = nullptr;

    uint32_t _pendingRebootAt = 0;
    bool _pendingFactoryReset = false;
    uint32_t _lastWsPush = 0;
    uint32_t _lastWsLog = 0;
    uint32_t _lastWsCleanup = 0;
    uint32_t _wsLogSeq = 0;

    AsyncWebServerRequest* _otaRequest = nullptr;
    bool _otaActive = false;
    bool _otaOk = false;
    String _otaError;

    void setupRoutes();
    void setupPortal();
    void setupWebSocket();
    void setupStatic();

    void addJsonRoute(const char* path,
                      bool requireAuth,
                      bool requireCsrf,
                      std::function<void(AsyncWebServerRequest*, JsonVariant&)> handler);

    bool requireAuth(AsyncWebServerRequest* request, bool csrf);
    String clientIp(AsyncWebServerRequest* request);
    String cookieToken(AsyncWebServerRequest* request);
    String headerValue(AsyncWebServerRequest* request, const char* name);

    void sendJson(AsyncWebServerRequest* request, int code, const JsonDocument& doc, const String& cookie = "");
    void sendRawJson(AsyncWebServerRequest* request, int code, const String& payload);
    void sendError(AsyncWebServerRequest* request, int code, const String& message);

    void scheduleReboot(uint32_t delayMs, bool factoryReset = false);
    String systemInfoJson();
    void pushStats();
    void pushLogs();

    void handleAuthStatus(AsyncWebServerRequest* request);
    void handleLogin(AsyncWebServerRequest* request, JsonVariant& json);
    void handleLogout(AsyncWebServerRequest* request);
    void handleSetup(AsyncWebServerRequest* request, JsonVariant& json);
    void handleChangePassword(AsyncWebServerRequest* request, JsonVariant& json);

    void handleSystemInfo(AsyncWebServerRequest* request);
    void handleSystemReboot(AsyncWebServerRequest* request);
    void handleSystemFactoryReset(AsyncWebServerRequest* request, JsonVariant& json);
    void handleSystemBackup(AsyncWebServerRequest* request);
    void handleSystemRestore(AsyncWebServerRequest* request, JsonVariant& json);
    void handleSystemLogs(AsyncWebServerRequest* request);
    void handleSystemTimeGet(AsyncWebServerRequest* request);
    void handleSystemTimeSet(AsyncWebServerRequest* request, JsonVariant& json);
    void handleOtaComplete(AsyncWebServerRequest* request);
    void handleOtaUpload(AsyncWebServerRequest* request,
                         const String& filename,
                         size_t index,
                         uint8_t* data,
                         size_t len,
                         bool final);

    void handleWifiScan(AsyncWebServerRequest* request);
    void handleWifiStatus(AsyncWebServerRequest* request);
    void handleWifiApSet(AsyncWebServerRequest* request, JsonVariant& json);
    void handleWifiWanSet(AsyncWebServerRequest* request, JsonVariant& json);
    void handleWifiWanDisconnect(AsyncWebServerRequest* request);
    void handleWifiClients(AsyncWebServerRequest* request);
    void handleClientBlock(AsyncWebServerRequest* request, JsonVariant& json);
    void handleClientRename(AsyncWebServerRequest* request, JsonVariant& json);

    void handleDhcpGet(AsyncWebServerRequest* request);
    void handleDhcpSet(AsyncWebServerRequest* request, JsonVariant& json);
    void handleNatGet(AsyncWebServerRequest* request);
    void handleNatSet(AsyncWebServerRequest* request, JsonVariant& json);
    void handlePing(AsyncWebServerRequest* request, JsonVariant& json);
};
