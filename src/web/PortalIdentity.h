#pragma once
#include <Arduino.h>
#include <IPAddress.h>

struct PortalIdentity {
    uint32_t ip;
    char cert[880];
    char key[320];
    bool loadOrCreate(IPAddress address);
};
