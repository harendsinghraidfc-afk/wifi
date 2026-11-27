#include "web/SecurePortal.h"
#include "web/PortalIdentity.h"
#include "core/PortalGate.h"
#include <WiFi.h>
#include <esp_https_server.h>
#include <lwip/sockets.h>

namespace {
PortalIdentity identity{};
httpd_handle_t server;
std::mutex identityMutex;
IPAddress bound;

IPAddress endpoint(int fd, bool peer) {
    sockaddr_storage addr{};
    socklen_t size = sizeof(addr);
    if ((peer ? getpeername(fd, (sockaddr*)&addr, &size) : getsockname(fd, (sockaddr*)&addr, &size)) != 0)
        return IPAddress();
    if (addr.ss_family == AF_INET) return IPAddress(((sockaddr_in*)&addr)->sin_addr.s_addr);
#if CONFIG_LWIP_IPV6
    if (addr.ss_family == AF_INET6) {
        auto* ip = &((sockaddr_in6*)&addr)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(ip)) return IPAddress(ip->un.u32_addr[3]);
    }
#endif
    return IPAddress();
}

esp_err_t handle(httpd_req_t* r) {
    int fd = httpd_req_to_sockfd(r);
    IPAddress ap = WiFi.softAPIP(), peer = endpoint(fd, true);
    if (!ap || !peer || endpoint(fd, false) != ap) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Local WiFi only");
    char host[64]{};
    String name = ap.toString();
    String path(r->uri);
    int query = path.indexOf('?');
    if (query >= 0) path.remove(query);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK ||
        (name != host && name + ":443" != host) || path == "/") {
        if (r->method != HTTP_GET) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use the portal IP");
        String location = "https://" + name + "/portal";
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Location", location.c_str());
        return httpd_resp_send(r, nullptr, 0);
    }
    if (r->method == HTTP_GET && path == "/portal") {
        extern const uint8_t pageStart[] asm("_binary_data_portal_html_start");
        extern const uint8_t pageEnd[] asm("_binary_data_portal_html_end");
        httpd_resp_set_type(r, "text/html; charset=utf-8");
        if (!LittleFS.exists("/portal.html")) return httpd_resp_send(r, (const char*)pageStart, pageEnd-pageStart);
        auto file = LittleFS.open("/portal.html", "r");
        if (!file) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Page unavailable");
        char buf[512];
        while (file.available()) {
            size_t n = file.readBytes(buf, sizeof(buf));
            if (!n || httpd_resp_send_chunk(r, buf, n) != ESP_OK) return ESP_FAIL;
        }
        return httpd_resp_send_chunk(r, nullptr, 0);
    }
    if (r->method == HTTP_GET && path == "/assets/app.css") {
        extern const uint8_t start[] asm("_binary_data_assets_app_css_start");
        extern const uint8_t end[] asm("_binary_data_assets_app_css_end");
        httpd_resp_set_type(r, "text/css");
        return httpd_resp_send(r, (const char*)start, end-start);
    }
    uint32_t revision = PortalGate::revision();
    bool enabled;
    auto settings = static_cast<ConfigStore*>(r->user_ctx)->portalSnapshot(enabled);
    JsonDocument doc;
    if (r->method == HTTP_GET && (path == "/api/portal/info" || path == "/api/portal/status")) {
        doc["enabled"] = enabled;
        doc["authorized"] = PortalGate::authorized(peer);
        doc["title"] = settings.title;
        doc["message"] = settings.message;
        doc["mode"] = settings.mode;
    } else if (r->method == HTTP_POST && path == "/api/portal/logout") {
        PortalGate::revoke(peer);
        doc["authorized"] = false;
    } else if (r->method == HTTP_POST && path == "/api/portal/authorize") {
        char body[512], contentType[64]{};
        if (httpd_req_get_hdr_value_str(r, "Content-Type", contentType, sizeof(contentType)) != ESP_OK ||
            strncasecmp(contentType, "application/json", 16) || r->content_len >= sizeof(body))
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Expected a small JSON object");
        size_t n = 0;
        while (n < r->content_len) {
            int count = httpd_req_recv(r, body+n, r->content_len-n);
            if (count <= 0) return ESP_FAIL;
            n += count;
        }
        if (deserializeJson(doc, body, n) || !doc.is<JsonObject>())
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        bool allow = enabled && (settings.mode == "click" ||
            (settings.mode == "code" && settings.code == (doc["code"] | "")) ||
            (settings.mode == "webhook" && SecurePortal::webhookAllows(settings.webhook, settings.secret, peer)));
        allow = allow && PortalGate::grant(peer, settings.minutes, revision);
        doc.clear();
        doc["authorized"] = allow;
        if (!allow) {
            doc["error"] = "Access denied";
            httpd_resp_set_status(r, "403 Forbidden");
        }
    } else return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    String result;
    serializeJson(doc, result);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, result.c_str(), result.length());
}
}

String SecurePortal::certificate() {
    std::lock_guard<std::mutex> lock(identityMutex);
    return server ? String(identity.cert) : String();
}

void SecurePortal::update(ConfigStore& store) {
    IPAddress ip = WiFi.softAPIP();
    if (ip == bound && server) return;
    std::lock_guard<std::mutex> lock(identityMutex);
    if (server) { httpd_ssl_stop(server); server = nullptr; }
    if (!ip || !identity.loadOrCreate(ip)) { log_e("Portal identity unavailable"); return; }
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.max_open_sockets = 2;
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
    cfg.tls_handshake_timeout_ms = 4000;
    cfg.servercert = (const uint8_t*)identity.cert;
    cfg.servercert_len = strlen(identity.cert)+1;
    cfg.prvtkey_pem = (const uint8_t*)identity.key;
    cfg.prvtkey_len = strlen(identity.key)+1;
    if (httpd_ssl_start(&server, &cfg) != ESP_OK) { server = nullptr; log_e("HTTPS start failed"); return; }
    for (auto method : {HTTP_GET, HTTP_POST}) {
        httpd_uri_t route{};
        route.uri = "/*";
        route.method = method;
        route.handler = handle;
        route.user_ctx = &store;
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            httpd_ssl_stop(server);
            server = nullptr;
            return;
        }
    }
    bound = ip;
}
