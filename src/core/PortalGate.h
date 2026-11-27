#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace PortalGate {
bool configure(bool enabled);
bool ready();
uint32_t revision();
bool authorized(IPAddress ip);
bool grant(IPAddress ip, int minutes, uint32_t expectedRevision = 0);
void revoke(IPAddress ip);
void disconnected(const String& mac);
}
