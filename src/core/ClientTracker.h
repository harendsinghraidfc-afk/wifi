#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/ConfigStore.h"

class ClientTracker {
public:
    void begin(ConfigStore* store);
    void loop();
    void watchDhcpNames();

    void onConnected(const String& mac);
    void onDisconnected(const String& mac);

    String json();
    bool block(const String& mac, bool blocked);
    bool rename(const String& mac, const String& name);
    int onlineCount();

private:
    struct Entry {
        String mac;
        String ip;
        String hostname;
        String name;
        int rssi = 0;
        bool blocked = false;
        bool online = false;
        uint32_t connectedAt = 0;
        uint32_t lastSeen = 0;
    };

    ConfigStore* _store = nullptr;
    std::map<String, Entry> _clients;
    uint32_t _lastPoll = 0;
    SemaphoreHandle_t _mutex = nullptr;
    QueueHandle_t _names = nullptr;

    void poll();
    void kickBlocked();
    void kickBlockedLocked();
    void assignIp(const uint8_t* mac, uint32_t ip);
    static String macToString(const uint8_t* mac);
    static String ipToString(uint32_t ip);
    static void ipEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
};
