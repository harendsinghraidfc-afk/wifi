#include "core/PortalDns.h"
#include "core/PortalGate.h"
#include <esp_random.h>

void PortalDns::loop(bool captive) {
    IPAddress ap = WiFi.softAPIP();
    if (!ap || bound != ap) {
        local.stop();
        upstream.stop();
        bound = IPAddress();
        if (!ap || !local.begin(ap, 53) || !upstream.begin(49152 + esp_random() % 16384)) return;
        bound = ap;
        for (auto& p : pending) p.port = 0;
    }
    int n = upstream.parsePacket();
    if (n > 0) {
        if (n <= sizeof(data) && upstream.read(data, n) == n && n >= 12 && (data[2] & 128)) {
            uint16_t id;
            memcpy(&id, data, 2);
            for (auto& p : pending) {
                if (p.port && p.proxyId == id && millis() - p.started < 4000 &&
                    upstream.remoteIP() == p.server && upstream.remotePort() == 53) {
                    memcpy(data, &p.originalId, 2);
                    local.beginPacket(p.ip, p.port);
                    local.write(data, n);
                    local.endPacket();
                    p.port = 0;
                    break;
                }
            }
        }
        upstream.clear();
    }
    n = local.parsePacket();
    if (n <= 0) return;
    IPAddress client = local.remoteIP();
    uint16_t port = local.remotePort();
    if (n > sizeof(data) || n < 17 || local.read(data, n) != n) { local.clear(); return; }
    if ((data[2] & 0xf8) || data[4] || data[5] != 1) return;
    if (!captive || PortalGate::authorized(client)) {
        IPAddress server = WiFi.dnsIP();
        if (!server || server == ap) return;
        for (auto& p : pending) {
            if (p.port && millis() - p.started < 4000) continue;
            uint16_t id = esp_random();
            for (const auto& other : pending) if (other.port && other.proxyId == id) return;
            p = {client, server, port, 0, id, millis()};
            memcpy(&p.originalId, data, 2);
            memcpy(data, &id, 2);
            upstream.beginPacket(server, 53);
            upstream.write(data, n);
            upstream.endPacket();
            break;
        }
        return;
    }
    size_t pos = 12;
    while (pos < n && data[pos]) {
        if (data[pos] > 63) return;
        pos += 1 + data[pos];
    }
    if (pos > 266 || pos + 5 > n) return;
    bool answer = data[pos+1] == 0 && (data[pos+2] == 1 || data[pos+2] == 255)
               && data[pos+3] == 0 && data[pos+4] == 1;
    pos += 5;
    data[2] = 0x84 | (data[2] & 1);
    data[3] = 0;
    memset(data + 6, 0, 6);
    if (answer) {
        data[7] = 1;
        const uint8_t rr[] = {0xc0,12,0,1,0,1,0,0,0,0,0,4,ap[0],ap[1],ap[2],ap[3]};
        memcpy(data + pos, rr, sizeof(rr));
        pos += sizeof(rr);
    }
    local.beginPacket(client, port);
    local.write(data, pos);
    local.endPacket();
}
