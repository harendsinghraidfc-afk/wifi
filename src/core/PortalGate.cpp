#include "core/PortalGate.h"
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/tcpip.h>

namespace {
struct PortalSession {
    uint8_t mac[6];
    uint32_t ip;
    int64_t expires;
};
PortalSession clients[10]{};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
netif_input_fn previous;
netif_linkoutput_fn previousOutput;
bool active;
bool installed;
uint32_t generation = 1;

bool matches(uint32_t ip, const uint8_t* mac) {
    const int64_t now = esp_timer_get_time();
    for (const auto& c : clients)
        if (c.ip == ip && c.expires > now && !memcmp(c.mac, mac, 6)) return true;
    return false;
}

// Existing NAT mappings may still receive UDP after expiry. Check the
// downstream leg too, while preserving DHCP, DNS and panel responses.
err_t filterOutput(netif* iface, pbuf* packet) {
    uint8_t h[34];
    bool allow = true;
    if (pbuf_copy_partial(packet, h, sizeof(h), 0) == sizeof(h) && h[12] == 8 && h[13] == 0) {
        uint32_t src, dst;
        memcpy(&src, h + 26, 4);
        memcpy(&dst, h + 30, 4);
        portENTER_CRITICAL(&lock);
        allow = !active || src == netif_ip4_addr(iface)->addr || matches(dst, h);
        portEXIT_CRITICAL(&lock);
    }
    return allow ? previousOutput(iface, packet) : ERR_OK;
}

err_t filter(pbuf* packet, netif* iface) {
    uint8_t h[34];
    size_t length = pbuf_copy_partial(packet, h, sizeof(h), 0);
    bool allow = false;
    portENTER_CRITICAL(&lock);
    if (!active) allow = true;
    else if (length >= 14 && h[12] == 8 && h[13] == 6) allow = true; // ARP
    else if (length == sizeof(h) && h[12] == 8 && h[13] == 0 && h[14] >> 4 == 4) {
        uint32_t src, dst;
        memcpy(&src, h + 26, 4);
        memcpy(&dst, h + 30, 4);
        allow = dst == netif_ip4_addr(iface)->addr || dst == UINT32_MAX;
        allow = allow || matches(src, h + 6);
    }
    portEXIT_CRITICAL(&lock);
    if (allow) return previous(packet, iface);
    pbuf_free(packet);
    return ERR_OK;
}
}

bool PortalGate::configure(bool enabled) {
    auto* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return !enabled;
    int index = esp_netif_get_netif_impl_index(ap);
    LOCK_TCPIP_CORE();
    auto* iface = netif_get_by_index(index);
    if (iface && iface->input != filter && enabled) {
        previous = iface->input;
        iface->input = filter;
    }
    if (iface && iface->linkoutput != filterOutput && enabled) {
        previousOutput = iface->linkoutput;
        iface->linkoutput = filterOutput;
    }
    portENTER_CRITICAL(&lock);
    installed = iface && iface->input == filter;
    active = enabled;
    ++generation;
    memset(clients, 0, sizeof(clients));
    portEXIT_CRITICAL(&lock);
    UNLOCK_TCPIP_CORE();
    return !enabled || installed;
}

bool PortalGate::ready() {
    portENTER_CRITICAL(&lock);
    bool ready = installed && active;
    portEXIT_CRITICAL(&lock);
    return ready;
}

bool PortalGate::authorized(IPAddress ip) {
    bool found = false;
    portENTER_CRITICAL(&lock);
    for (const auto& c : clients)
        if (c.ip == uint32_t(ip) && c.expires > esp_timer_get_time()) found = true;
    portEXIT_CRITICAL(&lock);
    return found;
}

uint32_t PortalGate::revision() {
    portENTER_CRITICAL(&lock);
    uint32_t value = generation;
    portEXIT_CRITICAL(&lock);
    return value;
}

bool PortalGate::grant(IPAddress ip, int minutes, uint32_t expectedRevision) {
    auto* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    wifi_sta_list_t stations{};
    if (!ap || esp_wifi_ap_get_sta_list(&stations) != ESP_OK || !stations.num || stations.num > 10) return false;
    // Active TCP clients already have an AP ARP entry even if changing
    // DHCP DNS options has just restarted the server and cleared its leases.
    uint8_t mac[6]{};
    ip4_addr_t target{uint32_t(ip)};
    int index = esp_netif_get_netif_impl_index(ap);
    LOCK_TCPIP_CORE();
    auto* iface = netif_get_by_index(index);
    eth_addr* eth = nullptr;
    const ip4_addr_t* cached = nullptr;
    if (iface && etharp_find_addr(iface, &target, &eth, &cached) >= 0) memcpy(mac, eth->addr, 6);
    UNLOCK_TCPIP_CORE();
    for (int i = 0; i < stations.num; ++i) {
        if (memcmp(stations.sta[i].mac, mac, 6)) continue;
        bool ok = false;
        portENTER_CRITICAL(&lock);
        PortalSession* slot = nullptr;
        for (auto& c : clients) {
            if (c.ip == uint32_t(ip)) { slot = &c; break; }
            if (c.expires <= esp_timer_get_time()) slot = &c;
        }
        if (active && installed && slot && (!expectedRevision || expectedRevision == generation)) {
            memcpy(slot->mac, mac, 6);
            slot->ip = uint32_t(ip);
            slot->expires = esp_timer_get_time() + minutes * 60000000LL;
            ok = true;
        }
        portEXIT_CRITICAL(&lock);
        return ok;
    }
    return false;
}

void PortalGate::revoke(IPAddress ip) {
    portENTER_CRITICAL(&lock);
    ++generation;
    for (auto& c : clients) if (c.ip == uint32_t(ip)) c = {};
    portEXIT_CRITICAL(&lock);
}

void PortalGate::disconnected(const String& mac) {
    uint8_t bytes[6];
    if (sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               bytes, bytes+1, bytes+2, bytes+3, bytes+4, bytes+5) != 6) return;
    portENTER_CRITICAL(&lock);
    ++generation;
    for (auto& c : clients) if (!memcmp(bytes, c.mac, 6)) c = {};
    portEXIT_CRITICAL(&lock);
}
