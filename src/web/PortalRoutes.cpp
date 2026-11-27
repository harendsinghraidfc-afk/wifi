#include "web/WebApp.h"
#include "web/SecurePortal.h"
#include "core/PortalGate.h"
#include <HTTPClient.h>
#include <atomic>
#include <memory>

namespace {
struct WebhookDecision {
    IPAddress ip;
    String url, secret;
    uint32_t revision;
    int minutes;
    std::atomic<int> result{0}; // 0 pending, 1 allow, -1 deny
};
std::atomic<bool> webhookBusy{false};
}

// A webhook decision is valid only for the device that initiated this request.
// Limit response size before parsing it on the router.
bool SecurePortal::webhookAllows(const String& url, const String& secret, IPAddress ip) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(1800);
    http.useHTTP10(true);
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");
    if (secret.length()) http.addHeader("Authorization", "Bearer " + secret);
    JsonDocument decision;
    decision["ip"] = ip.toString();
    String body;
    serializeJson(decision, body);
    int status = http.POST(body);
    if (status != 200 || http.getSize() > 512) return false;
    decision.clear();
    char reply[513];
    size_t n = 0;
    int size = http.getSize();
    uint32_t start = millis();
    while (n < sizeof(reply) && (size < 0 || n < size) && millis() - start < 1800) {
        if (client.available()) reply[n++] = client.read();
        else if (!client.connected()) break;
        else delay(1);
    }
    if (n == sizeof(reply) || (size >= 0 && n != size) || (size < 0 && client.connected())) return false;
    return !deserializeJson(decision, reply, n) && decision["allow"].is<bool>() && decision["allow"].as<bool>();
}

namespace {
void runWebhook(void* arg) {
    {
        // Release the worker's reference before FreeRTOS deletes its stack.
        std::unique_ptr<std::shared_ptr<WebhookDecision>> owner(static_cast<std::shared_ptr<WebhookDecision>*>(arg));
        auto& job = **owner;
        job.result.store(SecurePortal::webhookAllows(job.url, job.secret, job.ip) ? 1 : -1);
    }
    webhookBusy = false;
    vTaskDelete(nullptr);
}
}

void WebApp::setupPortal() {
    _server->on("/api/portal/certificate", HTTP_GET, [this](AsyncWebServerRequest* r) {
        if (!requireAuth(r, false)) return;
        String pem = SecurePortal::certificate();
        if (!pem.length()) return sendError(r, 503, "HTTPS portal unavailable");
        auto* res = r->beginResponse(200, "application/x-pem-file", pem);
        res->addHeader("Content-Disposition", "attachment; filename=\"lesh-portal.crt\"");
        res->addHeader("Cache-Control", "no-store");
        r->send(res);
    });
    auto info = [this](AsyncWebServerRequest* r) {
        const auto& cfg = _store->cfg();
        JsonDocument doc;
        doc["enabled"] = cfg.dhcp.captivePortal;
        doc["authorized"] = PortalGate::authorized(r->client()->remoteIP());
        doc["title"] = cfg.portal.title;
        doc["message"] = cfg.portal.message;
        doc["mode"] = cfg.portal.mode;
        sendJson(r, 200, doc);
    };
    _server->on("/api/portal/info", HTTP_GET, info);
    _server->on("/api/portal/status", HTTP_GET, info);
    addJsonRoute("/api/portal/authorize", false, false, [this](AsyncWebServerRequest* r, JsonVariant& j) {
        const auto& cfg = _store->cfg();
        IPAddress ip = r->client()->remoteIP();
        bool allow = cfg.dhcp.captivePortal && r->client()->localIP() == WiFi.softAPIP();
        if (allow && cfg.portal.mode == "code") allow = cfg.portal.code == (j["code"] | "");
        if (allow && cfg.portal.mode == "webhook") {
            if (webhookBusy.exchange(true)) return sendError(r, 429, "Webhook busy; retry shortly");
            auto job = std::make_shared<WebhookDecision>();
            job->ip = ip;
            job->url = cfg.portal.webhook;
            job->secret = cfg.portal.secret;
            job->minutes = cfg.portal.minutes;
            job->revision = PortalGate::revision();
            auto* owner = new(std::nothrow) std::shared_ptr<WebhookDecision>(job);
            if (!owner || xTaskCreate(runWebhook, "portal-hook", 6144, owner, 1, nullptr) != pdPASS) {
                delete owner;
                webhookBusy = false;
                return sendError(r, 503, "Webhook worker unavailable");
            }
            auto* res = r->beginChunkedResponse("application/json",
                [job, reply = String()](uint8_t* buf, size_t capacity, size_t sent) mutable -> size_t {
                    if (reply.isEmpty()) {
                        int result = job->result.load();
                        if (!result) return RESPONSE_TRY_AGAIN;
                        bool granted = result > 0 && PortalGate::grant(job->ip, job->minutes, job->revision);
                        reply = granted ? "{\"authorized\":true}" : "{\"authorized\":false,\"error\":\"Access denied\"}";
                    }
                    size_t n = std::min(capacity, reply.length() - sent);
                    memcpy(buf, reply.c_str() + sent, n);
                    return n;
                });
            res->addHeader("Cache-Control", "no-store");
            return r->send(res);
        }
        bool granted = allow && PortalGate::grant(ip, cfg.portal.minutes);
        JsonDocument doc;
        doc["authorized"] = granted;
        if (!granted) doc["error"] = allow ? "Could not identify this WiFi client" : "Access denied";
        sendJson(r, granted ? 200 : 403, doc);
    });
    _server->on("/api/portal/logout", HTTP_POST, [this](AsyncWebServerRequest* r) {
        PortalGate::revoke(r->client()->remoteIP());
        sendRawJson(r, 200, "{\"authorized\":false}");
    });
    _server->on("/api/portal/settings", HTTP_GET, [this](AsyncWebServerRequest* r) {
        if (!requireAuth(r, false)) return;
        const auto& cfg = _store->cfg();
        JsonDocument doc;
        doc["enabled"] = cfg.dhcp.captivePortal;
        doc["title"] = cfg.portal.title;
        doc["message"] = cfg.portal.message;
        doc["mode"] = cfg.portal.mode;
        doc["minutes"] = cfg.portal.minutes;
        doc["webhook"] = cfg.portal.webhook;
        doc["apIp"] = WiFi.softAPIP().toString();
        doc["custom"] = LittleFS.exists("/portal.html");
        sendJson(r, 200, doc);
    });
    addJsonRoute("/api/portal/settings", true, true, [this](AsyncWebServerRequest* r, JsonVariant& j) {
        auto next = _store->cfg();
        bool old = next.dhcp.captivePortal;
        next.dhcp.captivePortal = j["enabled"] | old;
        auto& p = next.portal;
        p.title = j["title"] | p.title;
        p.message = j["message"] | p.message;
        p.mode = j["mode"] | p.mode;
        p.minutes = j["minutes"] | p.minutes;
        p.code = j["code"] | p.code;
        p.apiKey = j["apiKey"] | p.apiKey;
        p.secret = j["secret"] | p.secret;
        p.webhook = j["webhook"] | p.webhook;
        String error;
        if (next.dhcp.captivePortal && !PortalGate::configure(true))
            return sendError(r, 503, "AP filter unavailable");
        if (!_store->apply(next, error)) {
            PortalGate::configure(old);
            return sendError(r, 400, error);
        }
        if (!next.dhcp.captivePortal) PortalGate::configure(false);
        if (old != next.dhcp.captivePortal) _dhcp->requestApply();
        sendRawJson(r, 200, "{\"ok\":true}");
    });
    addJsonRoute("/api/portal/grant", false, false, [this](AsyncWebServerRequest* r, JsonVariant& j) {
        const auto& p = _store->cfg().portal;
        if ((p.apiKey.isEmpty() || headerValue(r, "X-Portal-Key") != p.apiKey) && !requireAuth(r, true)) return;
        IPAddress ip;
        if (!ip.fromString(j["ip"] | "") || !PortalGate::grant(ip, p.minutes))
            return sendError(r, 400, "No matching connected WiFi client");
        sendRawJson(r, 200, "{\"authorized\":true}");
    });
    addJsonRoute("/api/portal/revoke", true, true, [this](AsyncWebServerRequest* r, JsonVariant& j) {
        IPAddress ip;
        if (!ip.fromString(j["ip"] | "")) return sendError(r, 400, "Invalid IP");
        PortalGate::revoke(ip);
        sendRawJson(r, 200, "{\"authorized\":false}");
    });
    _server->on("/api/portal/authorized", HTTP_GET, [this](AsyncWebServerRequest* r) {
        if (!requireAuth(r, false)) return;
        JsonDocument list, doc;
        deserializeJson(list, _clients->json());
        auto out = doc["clients"].to<JsonArray>();
        for (JsonObject entry : list["clients"].as<JsonArray>()) {
            IPAddress ip;
            if (entry["online"] && ip.fromString(entry["ip"] | "") && PortalGate::authorized(ip)) out.add(entry);
        }
        sendJson(r, 200, doc);
    });
    addJsonRoute("/api/portal/page", true, true, [this](AsyncWebServerRequest* r, JsonVariant& j) {
        if (j["reset"] == true) {
            if (LittleFS.exists("/portal.html") && !LittleFS.remove("/portal.html")) return sendError(r, 500, "Reset failed");
        } else {
            auto html = j["html"].as<JsonString>();
            if (!html.size() || html.size() > 8192) return sendError(r, 400, "HTML must be 1–8192 bytes");
            auto file = LittleFS.open("/portal.tmp", "w");
            if (!file || file.write((const uint8_t*)html.c_str(), html.size()) != html.size())
                return sendError(r, 500, "Unable to store HTML");
            file.close();
            if (!LittleFS.rename("/portal.tmp", "/portal.html")) return sendError(r, 500, "Unable to install HTML");
        }
        sendRawJson(r, 200, "{\"ok\":true}");
    });
    _server->on("/portal", HTTP_GET, [](AsyncWebServerRequest* r) {
        extern const uint8_t start[] asm("_binary_data_portal_html_start");
        extern const uint8_t end[] asm("_binary_data_portal_html_end");
        auto* response = LittleFS.exists("/portal.html") ? r->beginResponse(LittleFS, "/portal.html", "text/html") :
            r->beginResponse(200, "text/html", start, end - start);
        response->addHeader("Cache-Control", "no-store");
        r->send(response);
    });
}
