#pragma once

#include <Arduino.h>
#include <esp_netif.h>
#include "core/ConfigStore.h"

class DhcpManager {
public:
    void begin(ConfigStore* store);
    void loop();
    bool apply();
    void requestApply() { _applyPending = true; }
    String statusJson();

private:
    ConfigStore* _store = nullptr;
    bool _applied = false;
    bool _applyPending = false;
    String _dns;
};
