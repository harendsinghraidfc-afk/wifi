#include "modules/SystemTools.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>
#include <time.h>

static const char* TAG = "tools";
static const time_t MIN_VALID_TIME = 1700000000;

struct PingContext {
    SemaphoreHandle_t done = nullptr;
    uint32_t received = 0;
    uint32_t totalMs = 0;
    uint32_t minMs = UINT32_MAX;
    uint32_t maxMs = 0;
};

static void pingSuccess(esp_ping_handle_t handle, void* args) {
    auto* ctx = static_cast<PingContext*>(args);
    uint32_t elapsed = 0;
    if (esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed)) != ESP_OK) {
        elapsed = 0;
    }
    ctx->received++;
    ctx->totalMs += elapsed;
    if (elapsed < ctx->minMs) {
        ctx->minMs = elapsed;
    }
    if (elapsed > ctx->maxMs) {
        ctx->maxMs = elapsed;
    }
}

static void pingTimeout(esp_ping_handle_t, void*) {
}

static void pingEnd(esp_ping_handle_t, void* args) {
    auto* ctx = static_cast<PingContext*>(args);
    if (ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}

void SystemTools::begin(ConfigStore* store, WifiManager* wifi) {
    _store = store;
    _wifi = wifi;
    applyTime();
    log_i("System tools ready");
}

void SystemTools::applyTime() {
    if (!_store) {
        return;
    }

    const SystemConfig& cfg = _store->cfg().system;
    if (cfg.ntpEnabled) {
        configTime((long)cfg.tzOffsetMinutes * 60L, 0, cfg.ntpServer.c_str());
        log_i("NTP enabled: %s (offset %d min)", cfg.ntpServer.c_str(), cfg.tzOffsetMinutes);
    } else {
        log_i("NTP disabled");
    }
}

void SystemTools::loop() {
    if (!_store) {
        return;
    }

    uint32_t nowMs = millis();
    if ((uint32_t)(nowMs - _lastCheck) < 30000UL) {
        return;
    }
    _lastCheck = nowMs;

    const SystemConfig& cfg = _store->cfg().system;
    if (!cfg.scheduledReboot) {
        return;
    }

    time_t now = time(nullptr);
    if (now < MIN_VALID_TIME) {
        return;
    }

    struct tm local;
    localtime_r(&now, &local);

    char hhmm[6];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &local);
    if (cfg.scheduledRebootTime != hhmm) {
        return;
    }

    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &local);
    if (_lastRebootDate == date) {
        return;
    }

    _lastRebootDate = date;
    log_w("Scheduled reboot at %s", hhmm);
    delay(200);
    ESP.restart();
}

String SystemTools::ping(const String& host, int count) {
    JsonDocument doc;
    doc["host"] = host;

    count = constrain(count, 1, 10);

    IPAddress ip;
    if (!ip.fromString(host)) {
        if (!WiFi.hostByName(host.c_str(), ip)) {
            doc["error"] = "DNS lookup failed";
            String out;
            serializeJson(doc, out);
            return out;
        }
    }
    doc["ip"] = ip.toString();

    PingContext ctx;
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) {
        doc["error"] = "Out of memory";
        String out;
        serializeJson(doc, out);
        return out;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = count;
    config.interval_ms = 400;
    config.timeout_ms = 1000;
    config.target_addr.type = IPADDR_TYPE_V4;
    config.target_addr.u_addr.ip4.addr = (uint32_t)ip;

    esp_ping_callbacks_t callbacks = {};
    callbacks.cb_args = &ctx;
    callbacks.on_ping_success = pingSuccess;
    callbacks.on_ping_timeout = pingTimeout;
    callbacks.on_ping_end = pingEnd;

    esp_ping_handle_t session = nullptr;
    esp_err_t err = esp_ping_new_session(&config, &callbacks, &session);
    if (err != ESP_OK) {
        doc["error"] = String("Ping init failed: ") + esp_err_to_name(err);
        vSemaphoreDelete(ctx.done);
        String out;
        serializeJson(doc, out);
        return out;
    }

    err = esp_ping_start(session);
    if (err != ESP_OK) {
        doc["error"] = String("Ping start failed: ") + esp_err_to_name(err);
        esp_ping_delete_session(session);
        vSemaphoreDelete(ctx.done);
        String out;
        serializeJson(doc, out);
        return out;
    }

    uint32_t waitMs = (uint32_t)count * (config.interval_ms + config.timeout_ms) + 3000UL;
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
        doc["error"] = "Ping timed out";
    }

    esp_ping_stop(session);
    esp_ping_delete_session(session);
    vSemaphoreDelete(ctx.done);

    doc["sent"] = count;
    doc["received"] = ctx.received;
    doc["loss"] = count - ctx.received;
    doc["min"] = ctx.received ? ctx.minMs : 0;
    doc["avg"] = ctx.received ? (ctx.totalMs / ctx.received) : 0;
    doc["max"] = ctx.received ? ctx.maxMs : 0;

    String out;
    serializeJson(doc, out);
    return out;
}

String SystemTools::timeJson() {
    JsonDocument doc;
    time_t now = time(nullptr);

    doc["epoch"] = (uint32_t)now;
    doc["synced"] = now >= MIN_VALID_TIME;

    struct tm local;
    localtime_r(&now, &local);
    char formatted[32];
    strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &local);
    doc["local"] = formatted;

    if (_store) {
        doc["tzOffsetMinutes"] = _store->cfg().system.tzOffsetMinutes;
        doc["ntpEnabled"] = _store->cfg().system.ntpEnabled;
        doc["ntpServer"] = _store->cfg().system.ntpServer;
        doc["scheduledReboot"] = _store->cfg().system.scheduledReboot;
        doc["scheduledRebootTime"] = _store->cfg().system.scheduledRebootTime;
    }

    String out;
    serializeJson(doc, out);
    return out;
}
