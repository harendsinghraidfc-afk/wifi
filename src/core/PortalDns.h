#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>

class PortalDns {
public:
    void loop(bool captive);
private:
    struct Pending {
        IPAddress ip, server;
        uint16_t port, originalId, proxyId;
        uint32_t started;
    } pending[12]{};
    WiFiUDP local, upstream;
    IPAddress bound;
    uint8_t data[1500];
};
