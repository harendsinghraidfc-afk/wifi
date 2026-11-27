#pragma once

#include <Arduino.h>
#include "core/ConfigStore.h"
#include "core/WifiManager.h"

class SystemTools {
public:
    void begin(ConfigStore* store, WifiManager* wifi);
    void loop();

    void applyTime();
    String ping(const String& host, int count);
    String timeJson();

private:
    ConfigStore* _store = nullptr;
    WifiManager* _wifi = nullptr;
    uint32_t _lastCheck = 0;
    String _lastRebootDate;
};
