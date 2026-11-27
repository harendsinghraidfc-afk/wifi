#pragma once

#include <Arduino.h>
#include <esp_netif.h>
#include "core/ConfigStore.h"

class NatManager {
public:
    void begin(ConfigStore* store);
    void loop();

    bool supported() const { return _supported; }
    bool active() const { return _active; }
    void reapply() { _active = false; _lastCheck = 0; }
    String statusJson();

private:
    ConfigStore* _store = nullptr;
    bool _active = false;
    bool _supported = true;
    String _lastError;
    uint32_t _lastCheck = 0;

    bool enable();
    void disable();
};
