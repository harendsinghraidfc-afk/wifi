#include "web/WebApp.h"

#include <Update.h>
#include "core/PortalGate.h"
#include <WiFi.h>
#include <esp_log.h>
#include <esp_system.h>
#include <time.h>

#include "modules/LogBuffer.h"
#include "version.h"

static const char* TAG = "web";

static String strOr(JsonVariant& json, const char* key, const String& fallback) {
    JsonVariant value = json[key];
    if (value.is<const char*>()) {
        return String(value.as<const char*>());
    }
    return fallback;
}

void WebApp::begin(ConfigStore* store,
                   AuthManager* auth,
                   WifiManager* wifi,
                   NatManager* nat,
                   DhcpManager* dhcp,
                   ClientTracker* clients,
                   SystemTools* tools) {
    _store = store;
    _auth = auth;
    _wifi = wifi;
    _nat = nat;
    _dhcp = dhcp;
    _clients = clients;
    _tools = tools;

    _server = new AsyncWebServer(80);
    _ws = new AsyncWebSocket("/ws");

    setupWebSocket();
    setupRoutes();
    setupStatic();

    _server->begin();
    log_i("HTTP server started on port 80");
}

void WebApp::loop() {
    uint32_t now = millis();

    if (_pendingRebootAt != 0 && (int32_t)(now - _pendingRebootAt) >= 0) {
        _pendingRebootAt = 0;
        if (_pendingFactoryReset) {
            _pendingFactoryReset = false;
            _store->factoryReset();
        } else {
            log_w("Rebooting device");
            delay(100);
            ESP.restart();
        }
    }

    if (!_ws) {
        return;
    }

    if ((uint32_t)(now - _lastWsCleanup) > 10000UL) {
        _lastWsCleanup = now;
        _ws->cleanupClients();
    }

    if (_ws->count() > 0) {
        if ((uint32_t)(now - _lastWsPush) >= 2000UL) {
            _lastWsPush = now;
            pushStats();
        }
        if ((uint32_t)(now - _lastWsLog) >= 1000UL) {
            _lastWsLog = now;
            pushLogs();
        }
    }
}

void WebApp::setupWebSocket() {
    _ws->handleHandshake([this](AsyncWebServerRequest* request) {
        String token = cookieToken(request);
        bool ok = _auth->validate(token);
        if (!ok) {
            log_w("WebSocket handshake rejected");
        }
        return ok;
    });

    _ws->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*, size_t) {
        if (type == WS_EVT_CONNECT) {
            log_i("WebSocket client %u connected from %s", client->id(), client->remoteIP().toString().c_str());
        } else if (type == WS_EVT_DISCONNECT) {
            log_i("WebSocket client %u disconnected", client->id());
        }
    });

    _server->addHandler(_ws);
}

void WebApp::addJsonRoute(const char* path,
                          bool requireAuthFlag,
                          bool requireCsrf,
                          std::function<void(AsyncWebServerRequest*, JsonVariant&)> handler) {
    auto* route = new AsyncCallbackJsonWebHandler(path,
        [this, requireAuthFlag, requireCsrf, handler](AsyncWebServerRequest* request, JsonVariant& json) {
            if (requireAuthFlag && !requireAuth(request, requireCsrf)) {
                return;
            }
            handler(request, json);
        });
    route->setMethod(HTTP_POST);
    _server->addHandler(route);
}

void WebApp::setupRoutes() {
    setupPortal();
    _server->on("/api/auth/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleAuthStatus(request);
    });

    addJsonRoute("/api/auth/login", false, false, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleLogin(request, json);
    });

    _server->on("/api/auth/logout", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleLogout(request);
    });

    addJsonRoute("/api/setup", false, false, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleSetup(request, json);
    });

    addJsonRoute("/api/auth/password", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleChangePassword(request, json);
    });

    _server->on("/api/system/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!requireAuth(request, false)) return;
        handleSystemInfo(request);
    });

    _server->on("/api/system/reboot", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleSystemReboot(request);
    });

    addJsonRoute("/api/system/factory-reset", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleSystemFactoryReset(request, json);
    });

    _server->on("/api/system/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleSystemBackup(request);
    });

    addJsonRoute("/api/system/config", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleSystemRestore(request, json);
    });

    _server->on("/api/system/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleSystemLogs(request);
    });

    _server->on("/api/system/time", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleSystemTimeGet(request);
    });

    addJsonRoute("/api/system/time", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleSystemTimeSet(request, json);
    });

    _server->on("/api/system/ota", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            handleOtaComplete(request);
        },
        [this](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            handleOtaUpload(request, filename, index, data, len, final);
        });

    _server->on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleWifiScan(request);
    });

    _server->on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleWifiStatus(request);
    });

    addJsonRoute("/api/wifi/ap", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleWifiApSet(request, json);
    });

    addJsonRoute("/api/wifi/wan", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleWifiWanSet(request, json);
    });

    _server->on("/api/wifi/wan/disconnect", HTTP_POST, [this](AsyncWebServerRequest* request) {
        handleWifiWanDisconnect(request);
    });

    _server->on("/api/wifi/clients", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleWifiClients(request);
    });

    addJsonRoute("/api/wifi/clients/block", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleClientBlock(request, json);
    });

    addJsonRoute("/api/wifi/clients/rename", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleClientRename(request, json);
    });

    _server->on("/api/dhcp", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleDhcpGet(request);
    });

    addJsonRoute("/api/dhcp", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleDhcpSet(request, json);
    });

    _server->on("/api/router/nat", HTTP_GET, [this](AsyncWebServerRequest* request) {
        handleNatGet(request);
    });

    addJsonRoute("/api/router/nat", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handleNatSet(request, json);
    });

    addJsonRoute("/api/tools/ping", true, true, [this](AsyncWebServerRequest* request, JsonVariant& json) {
        handlePing(request, json);
    });
}

void WebApp::setupStatic() {
    extern const uint8_t indexStart[] asm("_binary_data_index_html_start");
    extern const uint8_t indexEnd[] asm("_binary_data_index_html_end");
    extern const uint8_t cssStart[] asm("_binary_data_assets_app_css_start");
    extern const uint8_t cssEnd[] asm("_binary_data_assets_app_css_end");
    extern const uint8_t jsStart[] asm("_binary_data_assets_app_js_start");
    extern const uint8_t jsEnd[] asm("_binary_data_assets_app_js_end");
    auto serve = [this](const char* path, const char* mime, const uint8_t* start, size_t size) {
        _server->on(path, HTTP_GET, [mime, start, size](AsyncWebServerRequest* request) {
            auto* response = request->beginResponse(200, mime, start, size);
            response->addHeader("Cache-Control", "no-cache");
            request->send(response);
        });
    };
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest* r) {
        if (_store->cfg().dhcp.captivePortal && !PortalGate::authorized(r->client()->remoteIP())) {
            r->redirect("http://" + _wifi->apIp() + "/portal");
            return;
        }
        r->send(200, "text/html", indexStart, indexEnd - indexStart);
    });
    serve("/admin", "text/html", indexStart, indexEnd - indexStart);
    serve("/index.html", "text/html", indexStart, indexEnd - indexStart);
    serve("/assets/app.css", "text/css", cssStart, cssEnd - cssStart);
    serve("/assets/app.js", "application/javascript", jsStart, jsEnd - jsStart);

    _server->on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(204);
    });

    _server->onNotFound([this](AsyncWebServerRequest* request) {
        if ((_store->setupRequired() || (_store->cfg().dhcp.captivePortal && !PortalGate::authorized(request->client()->remoteIP()))) &&
            request->method() == HTTP_GET && !request->url().startsWith("/api/")) {
            request->redirect("http://" + _wifi->apIp() + (_store->setupRequired() ? "/admin" : "/portal"));
            return;
        }
        if (request->url().startsWith("/api/")) {
            sendError(request, 404, "Not found");
            return;
        }
        // Keep OS captive-portal probes quiet once setup is complete.
        if (request->url() == "/connecttest.txt") return request->send(200, "text/plain", "Microsoft Connect Test");
        if (request->url() == "/ncsi.txt") return request->send(200, "text/plain", "Microsoft NCSI");
        if (request->url() == "/hotspot-detect.html") return request->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        request->send(204);
    });
}

bool WebApp::requireAuth(AsyncWebServerRequest* request, bool csrf) {
    String token = cookieToken(request);
    if (!_auth->validate(token)) {
        sendError(request, 401, "Not authenticated");
        return false;
    }
    if (csrf) {
        String provided = headerValue(request, "X-CSRF-Token");
        if (provided.isEmpty() || provided != _auth->csrfFor(token)) {
            sendError(request, 403, "Invalid CSRF token");
            return false;
        }
    }
    return true;
}

String WebApp::clientIp(AsyncWebServerRequest* request) {
    if (request->client()) {
        return request->client()->remoteIP().toString();
    }
    return "unknown";
}

String WebApp::cookieToken(AsyncWebServerRequest* request) {
    return AuthManager::cookieValue(headerValue(request, "Cookie"), "SID");
}

String WebApp::headerValue(AsyncWebServerRequest* request, const char* name) {
    const AsyncWebHeader* header = request->getHeader(name);
    return header ? header->value() : String();
}

void WebApp::sendJson(AsyncWebServerRequest* request, int code, const JsonDocument& doc, const String& cookie) {
    String payload;
    serializeJson(doc, payload);
    auto* response = request->beginResponse(code, "application/json", payload);
    if (cookie.length()) response->addHeader("Set-Cookie", cookie);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void WebApp::sendRawJson(AsyncWebServerRequest* request, int code, const String& payload) {
    request->send(code, "application/json", payload);
}

void WebApp::sendError(AsyncWebServerRequest* request, int code, const String& message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(request, code, doc);
}

void WebApp::scheduleReboot(uint32_t delayMs, bool factoryReset) {
    _pendingRebootAt = millis() + (delayMs == 0 ? 1 : delayMs);
    _pendingFactoryReset = factoryReset;
}

String WebApp::systemInfoJson() {
    JsonDocument doc;
    doc["version"] = ESP_ROUTER_VERSION;
    doc["build"] = __DATE__ " " __TIME__;
    doc["chip"] = ESP.getChipModel();
    doc["chipRevision"] = ESP.getChipRevision();
    doc["cores"] = ESP.getChipCores();
    doc["mac"] = WiFi.macAddress();
    doc["hostname"] = _store->cfg().system.hostname;
    doc["uptime"] = (uint32_t)(millis() / 1000UL);
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["sdk"] = ESP.getSdkVersion();
    doc["resetReason"] = (int)esp_reset_reason();
    doc["time"] = (uint32_t)time(nullptr);
    doc["apIp"] = _wifi->apIp();
    doc["staIp"] = _wifi->staIp();
    doc["naptSupported"] = _nat->supported();
    doc["naptActive"] = _nat->active();
    doc["bandSupported"] = _wifi->bandSupported();
    doc["setupRequired"] = _store->setupRequired();

    String out;
    serializeJson(doc, out);
    return out;
}

void WebApp::handleAuthStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["setupRequired"] = _store->setupRequired();
    String token = cookieToken(request);
    bool authenticated = _auth->validate(token);
    doc["authenticated"] = authenticated;
    doc["username"] = "Lesh";
    doc["version"] = ESP_ROUTER_VERSION;
    doc["hostname"] = _store->cfg().system.hostname;
    if (authenticated) {
        doc["csrf"] = _auth->csrfFor(token);
    }
    sendJson(request, 200, doc);
}

void WebApp::handleLogin(AsyncWebServerRequest* request, JsonVariant& json) {
    if (_store->setupRequired()) {
        sendError(request, 409, "Device setup is not complete");
        return;
    }

    String ip = clientIp(request);
    uint32_t retryAfter = 0;
    if (_auth->isLocked(ip, retryAfter)) {
        JsonDocument doc;
        doc["error"] = "Too many failed attempts";
        doc["retryAfter"] = retryAfter;
        sendJson(request, 429, doc);
        return;
    }

    String password = json["password"] | "";
    if (String(json["username"] | "") != "Lesh" || !_auth->checkAdminPassword(password)) {
        _auth->noteFailure(ip);
        sendError(request, 401, "Incorrect password");
        return;
    }

    _auth->noteSuccess(ip);
    String token = _auth->createSession(ip);
    LogBuffer::write('I', "Admin signed in from %s", ip.c_str());

    JsonDocument doc;
    doc["ok"] = true;
    doc["username"] = "Lesh";
    doc["csrf"] = _auth->csrfFor(token);
    sendJson(request, 200, doc, AuthManager::makeCookie(token));
}

void WebApp::handleLogout(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    String token = cookieToken(request);
    _auth->logout(token);

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc, AuthManager::makeCookie("", true));
}

void WebApp::handleSetup(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!_store->setupRequired()) {
        sendError(request, 409, "Device is already configured");
        return;
    }

    String adminPassword = json["adminPassword"] | "";
    String apSsid = json["apSsid"] | "";
    String apPassword = json["apPassword"] | "";
    String wanSsid = json["wanSsid"] | "";
    String wanPassword = json["wanPassword"] | "";
    bool natEnabled = json["nat"] | true;

    if (adminPassword.length() < 8 || adminPassword.length() > 64) {
        sendError(request, 400, "Admin password must be 8-64 characters");
        return;
    }
    if (apSsid.isEmpty() || apSsid.length() > 32) {
        sendError(request, 400, "WiFi name must be 1-32 characters");
        return;
    }
    if (apPassword.length() < 8 || apPassword.length() > 63) {
        sendError(request, 400, "WiFi password must be 8-63 characters");
        return;
    }
    if (wanSsid.length() > 32 || wanPassword.length() > 63) {
        sendError(request, 400, "Invalid upstream WiFi credentials");
        return;
    }

    RouterConfig next = _store->cfg();
    next.ap.ssid = apSsid;
    next.wan.ssid = wanSsid;
    next.wan.enabled = !wanSsid.isEmpty();
    next.natEnabled = natEnabled;

    String error;
    if (!_store->apply(next, error)) {
        sendError(request, 400, error);
        return;
    }

    if (!_auth->setAdminPassword(adminPassword)) {
        sendError(request, 500, "Failed to store admin password");
        return;
    }

    _store->secrets().apPassword = apPassword;
    _store->secrets().wanPassword = wanPassword;
    _store->saveSecrets();

    JsonDocument doc;
    doc["ok"] = true;
    doc["reboot"] = true;
    sendJson(request, 200, doc);
    scheduleReboot(1500);
}

void WebApp::handleChangePassword(AsyncWebServerRequest* request, JsonVariant& json) {
    String current = json["current"] | "";
    String next = json["newPassword"] | "";

    if (next.length() < 8 || next.length() > 64) {
        sendError(request, 400, "New password must be 8-64 characters");
        return;
    }
    if (!_auth->changeAdminPassword(current, next)) {
        sendError(request, 403, "Current password is incorrect");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleSystemInfo(AsyncWebServerRequest* request) {
    sendRawJson(request, 200, systemInfoJson());
}

void WebApp::handleSystemReboot(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["reboot"] = true;
    sendJson(request, 200, doc);
    scheduleReboot(800);
}

void WebApp::handleSystemFactoryReset(AsyncWebServerRequest* request, JsonVariant& json) {
    String confirm = json["confirm"] | "";
    if (confirm != "RESET") {
        sendError(request, 400, "Type RESET to confirm");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["reboot"] = true;
    sendJson(request, 200, doc);
    scheduleReboot(1200, true);
}

void WebApp::handleSystemBackup(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }

    bool includeSecrets = request->hasParam("secrets") && request->getParam("secrets")->value() == "1";
    String payload = _store->backupJson(includeSecrets);

    auto* response = request->beginResponse(200, "application/json", payload);
    response->addHeader("Content-Disposition", "attachment; filename=\"esp-router-backup.json\"");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void WebApp::handleSystemRestore(AsyncWebServerRequest* request, JsonVariant& json) {
    String raw;
    serializeJson(json, raw);

    String error;
    if (!_store->restoreJson(raw, error)) {
        sendError(request, 400, error);
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["rebootRequired"] = true;
    doc["reboot"] = true;
    sendJson(request, 200, doc);
    scheduleReboot(1200);
}

void WebApp::handleSystemLogs(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }

    uint32_t since = 0;
    if (request->hasParam("since")) {
        since = (uint32_t)request->getParam("since")->value().toInt();
    }
    sendRawJson(request, 200, LogBuffer::jsonSince(since));
}

void WebApp::handleSystemTimeGet(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    sendRawJson(request, 200, _tools->timeJson());
}

void WebApp::handleSystemTimeSet(AsyncWebServerRequest* request, JsonVariant& json) {
    RouterConfig next = _store->cfg();
    next.system.tzOffsetMinutes = json["tzOffsetMinutes"] | next.system.tzOffsetMinutes;
    next.system.ntpEnabled = json["ntpEnabled"] | next.system.ntpEnabled;
    next.system.ntpServer = strOr(json, "ntpServer", next.system.ntpServer);
    next.system.scheduledReboot = json["scheduledReboot"] | next.system.scheduledReboot;
    next.system.scheduledRebootTime = strOr(json, "scheduledRebootTime", next.system.scheduledRebootTime);

    String error;
    if (!_store->apply(next, error)) {
        sendError(request, 400, error);
        return;
    }

    _tools->applyTime();

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleOtaComplete(AsyncWebServerRequest* request) {
    if (request != _otaRequest) {
        if (requireAuth(request, true))
            sendError(request, 409, "No firmware upload available for this request");
        return;
    }
    _otaRequest = nullptr;
    if (_otaActive) {
        Update.abort();
        _otaActive = false;
    }

    if (!_otaError.isEmpty()) {
        String error = _otaError;
        _otaError = "";
        sendError(request, 400, error);
        return;
    }

    if (!_otaOk) {
        sendError(request, 400, "Firmware update failed");
        return;
    }

    _otaOk = false;

    JsonDocument doc;
    doc["ok"] = true;
    doc["reboot"] = true;
    sendJson(request, 200, doc);
    scheduleReboot(2500);
}

void WebApp::handleOtaUpload(AsyncWebServerRequest* request,
                             const String& filename,
                             size_t index,
                             uint8_t* data,
                             size_t len,
                             bool final) {
    if (index == 0) {
        if (_otaRequest || _pendingRebootAt) return;

        String token = cookieToken(request);
        String csrf = headerValue(request, "X-CSRF-Token");
        if (!_auth->validate(token) || csrf.isEmpty() || csrf != _auth->csrfFor(token)) {
            return;
        }

        _otaRequest = request;
        _otaOk = false;
        _otaError = "";
        request->onDisconnect([this, request]() {
            if (_otaRequest != request) return;
            if (_otaActive) Update.abort();
            _otaRequest = nullptr;
            _otaActive = false;
        });
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            _otaError = Update.errorString();
            return;
        }
        _otaActive = true;
        log_i("OTA update started: %s", filename.c_str());
    }

    if (_otaRequest != request || !_otaActive) {
        return;
    }

    if (len > 0) {
        if (Update.write(data, len) != len) {
            _otaError = Update.errorString();
            Update.abort();
            _otaActive = false;
            log_e("OTA write failed: %s", _otaError.c_str());
            return;
        }
    }

    if (final) {
        if (Update.end(true)) {
            _otaOk = true;
            log_i("OTA update finished successfully");
        } else {
            _otaError = Update.errorString();
            log_e("OTA finalize failed: %s", _otaError.c_str());
        }
        _otaActive = false;
    }
}

void WebApp::handleWifiScan(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    sendRawJson(request, 200, _wifi->scanJson());
}

void WebApp::handleWifiStatus(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    sendRawJson(request, 200, _wifi->statusJson());
}

void WebApp::handleWifiApSet(AsyncWebServerRequest* request, JsonVariant& json) {
    RouterConfig next = _store->cfg();
    next.ap.ssid = strOr(json, "ssid", next.ap.ssid);
    next.ap.channel = json["channel"] | next.ap.channel;
    next.ap.hidden = json["hidden"] | next.ap.hidden;
    next.ap.maxClients = json["maxClients"] | next.ap.maxClients;
    next.ap.txPower = json["txPower"] | next.ap.txPower;
    next.ap.band = strOr(json, "band", next.ap.band);
    next.ap.ip = strOr(json, "ip", next.ap.ip);
    next.ap.subnet = strOr(json, "subnet", next.ap.subnet);

    bool passwordChanged = false;
    String newPassword;
    if (json["password"].is<const char*>()) {
        newPassword = json["password"].as<const char*>();
        if (!newPassword.isEmpty() && (newPassword.length() < 8 || newPassword.length() > 63)) {
            sendError(request, 400, "WiFi password must be 8-63 characters, or empty for an open network");
            return;
        }
        passwordChanged = true;
    }

    // Moving the AP to a new subnet invalidates the old DHCP pool; shift it
    // automatically (x.x.x.10 - x.x.x.100) so the save cannot dead-end.
    const ApConfig& oldAp = _store->cfg().ap;
    if (next.ap.ip != oldAp.ip || next.ap.subnet != oldAp.subnet) {
        IPAddress ip, mask;
        if (ip.fromString(next.ap.ip) && mask.fromString(next.ap.subnet)) {
            uint32_t base = (uint32_t)ip & (uint32_t)mask;
            uint32_t broadcast = base | ~(uint32_t)mask;
            uint32_t start = base + 10;
            uint32_t end = base + 100;
            if (end >= broadcast) {
                end = broadcast - 1;
            }
            if (start == (uint32_t)ip) {
                start++;
            }
            if (end == (uint32_t)ip) {
                end--;
            }
            if (start < end && start > base) {
                IPAddress poolStart(start), poolEnd(end);
                next.dhcp.start = poolStart.toString();
                next.dhcp.end = poolEnd.toString();
            }
        }
    }

    String error;
    if (!_store->apply(next, error)) {
        sendError(request, 400, error);
        return;
    }

    if (passwordChanged) {
        _store->secrets().apPassword = newPassword;
        _store->saveSecrets();
    }

    _wifi->scheduleApRestart(800);

    JsonDocument doc;
    doc["ok"] = true;
    doc["apRestart"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleWifiWanSet(AsyncWebServerRequest* request, JsonVariant& json) {
    RouterConfig next = _store->cfg();
    next.wan.ssid = strOr(json, "ssid", next.wan.ssid);
    next.wan.useStatic = json["useStatic"] | next.wan.useStatic;
    next.wan.ip = strOr(json, "ip", next.wan.ip);
    next.wan.gateway = strOr(json, "gateway", next.wan.gateway);
    next.wan.subnet = strOr(json, "subnet", next.wan.subnet);
    next.wan.dns1 = strOr(json, "dns1", next.wan.dns1);
    next.wan.dns2 = strOr(json, "dns2", next.wan.dns2);
    next.wan.enabled = json["enabled"] | true;

    bool passwordChanged = false;
    String newPassword;
    if (json["password"].is<const char*>()) {
        newPassword = json["password"].as<const char*>();
        if (newPassword.length() > 63) {
            sendError(request, 400, "Upstream password must be at most 63 characters");
            return;
        }
        passwordChanged = true;
    }

    String error;
    if (!_store->apply(next, error)) {
        sendError(request, 400, error);
        return;
    }

    if (passwordChanged) {
        _store->secrets().wanPassword = newPassword;
        _store->saveSecrets();
    }

    if (next.wan.enabled && !next.wan.ssid.isEmpty()) {
        String connectError;
        if (!_wifi->connectWan(connectError)) {
            sendError(request, 400, connectError);
            return;
        }
    } else {
        _wifi->disconnectWan();
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["connecting"] = next.wan.enabled;
    sendJson(request, 200, doc);
}

void WebApp::handleWifiWanDisconnect(AsyncWebServerRequest* request) {
    if (!requireAuth(request, true)) {
        return;
    }

    RouterConfig next = _store->cfg();
    next.wan.enabled = false;

    String error;
    if (!_store->apply(next, error)) {
        sendError(request, 400, error);
        return;
    }

    _wifi->disconnectWan();

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleWifiClients(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    sendRawJson(request, 200, _clients->json());
}

void WebApp::handleClientBlock(AsyncWebServerRequest* request, JsonVariant& json) {
    String mac = json["mac"] | "";
    bool blocked = json["blocked"] | true;

    if (mac.isEmpty() || !_clients->block(mac, blocked)) {
        sendError(request, 400, "Invalid client");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleClientRename(AsyncWebServerRequest* request, JsonVariant& json) {
    String mac = json["mac"] | "";
    String name = json["name"] | "";

    if (mac.isEmpty() || !_clients->rename(mac, name)) {
        sendError(request, 400, "Invalid client");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleDhcpGet(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    sendRawJson(request, 200, _dhcp->statusJson());
}

void WebApp::handleDhcpSet(AsyncWebServerRequest* request, JsonVariant& json) {
    RouterConfig next = _store->cfg();
    next.dhcp.start = strOr(json, "start", next.dhcp.start);
    next.dhcp.end = strOr(json, "end", next.dhcp.end);
    next.dhcp.leaseMinutes = json["leaseMinutes"] | next.dhcp.leaseMinutes;
    next.dhcp.captivePortal = json["captivePortal"] | next.dhcp.captivePortal;
    bool changed = next.dhcp.captivePortal != _store->cfg().dhcp.captivePortal;
    if (changed && next.dhcp.captivePortal && !PortalGate::configure(true))
        return sendError(request, 503, "AP filter unavailable");

    String error;
    if (!_store->apply(next, error)) {
        if (changed && next.dhcp.captivePortal) PortalGate::configure(false);
        sendError(request, 400, error);
        return;
    }

    if (changed && !next.dhcp.captivePortal) PortalGate::configure(false);
    if (!_dhcp->apply()) {
        sendError(request, 500, "Failed to apply DHCP settings");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handleNatGet(AsyncWebServerRequest* request) {
    if (!requireAuth(request, false)) {
        return;
    }
    sendRawJson(request, 200, _nat->statusJson());
}

void WebApp::handleNatSet(AsyncWebServerRequest* request, JsonVariant& json) {
    RouterConfig next = _store->cfg();
    next.natEnabled = json["enabled"] | next.natEnabled;

    String error;
    if (!_store->apply(next, error)) {
        sendError(request, 400, error);
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(request, 200, doc);
}

void WebApp::handlePing(AsyncWebServerRequest* request, JsonVariant& json) {
    String host = json["host"] | "";
    int count = json["count"] | 4;

    if (host.isEmpty()) {
        sendError(request, 400, "Host is required");
        return;
    }

    sendRawJson(request, 200, _tools->ping(host, count));
}

void WebApp::pushStats() {
    if (!_ws || _ws->count() == 0) {
        return;
    }

    JsonDocument doc;
    doc["type"] = "stats";
    doc["uptime"] = (uint32_t)(millis() / 1000UL);
    doc["heap"] = ESP.getFreeHeap();
    doc["staConnected"] = _wifi->staConnected();
    doc["staIp"] = _wifi->staIp();
    doc["staRssi"] = _wifi->staRssi();
    doc["apClients"] = _wifi->apClientCount();
    doc["onlineClients"] = _clients->onlineCount();
    doc["natActive"] = _nat->active();
    doc["channel"] = _wifi->effectiveChannel();
    doc["time"] = (uint32_t)time(nullptr);

    String payload;
    serializeJson(doc, payload);
    _ws->textAll(payload);
}

void WebApp::pushLogs() {
    if (!_ws || _ws->count() == 0) {
        return;
    }

    JsonDocument doc;
    uint32_t latest = 0;
    LogBuffer::fill(doc, _wsLogSeq, latest);

    JsonArray lines = doc["lines"].as<JsonArray>();
    if (lines.isNull() || lines.size() == 0) {
        _wsLogSeq = latest;
        return;
    }

    doc["type"] = "logs";
    _wsLogSeq = latest;

    String payload;
    serializeJson(doc, payload);
    _ws->textAll(payload);
}
