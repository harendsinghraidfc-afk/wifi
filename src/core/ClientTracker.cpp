#include "core/ClientTracker.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/etharp.h>
#include <lwip/raw.h>
#include <lwip/tcpip.h>
#include <lwip/prot/dhcp.h>

static const char* TAG = "clients";

namespace {
struct ClientHostname {
    uint8_t mac[6];
    char name[64];
};

raw_pcb* nameListener = nullptr; // Accessed only on the lwIP thread.

// The SDK lease event omits option 12. Read just that option from AP DHCP
// requests and leave the original packet for the normal DHCP server.
u8_t readDhcpName(void* queue, raw_pcb*, pbuf* packet, const ip_addr_t*) {
    unsigned udp = ip_current_header_tot_len();
    unsigned bootp = udp + 8;
    if (packet->tot_len < bootp + 240 || pbuf_get_at(packet, udp) != 0 ||
        pbuf_get_at(packet, udp + 1) != 68 || pbuf_get_at(packet, udp + 2) != 0 ||
        pbuf_get_at(packet, udp + 3) != 67 || pbuf_get_at(packet, bootp) != 1 ||
        pbuf_get_at(packet, bootp + 1) != 1 || pbuf_get_at(packet, bootp + 2) != 6) return 0;

    uint8_t cookie[4];
    pbuf_copy_partial(packet, cookie, sizeof(cookie), bootp + 236);
    if (memcmp(cookie, "\x63\x82\x53\x63", 4) != 0) return 0;
    unsigned pos = bootp + 240;
    while (pos < packet->tot_len) {
        uint8_t option = pbuf_get_at(packet, pos++);
        if (option == 0) continue;
        if (option == 255 || pos >= packet->tot_len) break;
        unsigned len = pbuf_get_at(packet, pos++);
        if (pos + len > packet->tot_len) break;
        if (option == DHCP_OPTION_HOSTNAME && len && len < sizeof(ClientHostname::name)) {
            ClientHostname record{};
            pbuf_copy_partial(packet, record.mac, sizeof(record.mac), bootp + 28);
            pbuf_copy_partial(packet, record.name, len, pos);
            for (unsigned i = 0; i < len; ++i) {
                if (static_cast<unsigned char>(record.name[i]) < 32) return 0;
            }
            xQueueSend(static_cast<QueueHandle_t>(queue), &record, 0);
            break;
        }
        pos += len;
    }
    return 0;
}

// The web task (JSON/rename/block) and the WiFi/IP event tasks both touch the
// client map, so every access goes through a recursive mutex.
class ScopedLock {
public:
    explicit ScopedLock(SemaphoreHandle_t mutex) : _mutex(mutex) {
        if (_mutex) {
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        }
    }
    ~ScopedLock() {
        if (_mutex) {
            xSemaphoreGiveRecursive(_mutex);
        }
    }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    SemaphoreHandle_t _mutex;
};
}  // namespace

void ClientTracker::begin(ConfigStore* store) {
    _store = store;
    _names = xQueueCreate(12, sizeof(ClientHostname));
    if (!_mutex) {
        _mutex = xSemaphoreCreateRecursiveMutex();
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &ClientTracker::ipEvent, this);
    log_i("Client tracker ready");
}

void ClientTracker::watchDhcpNames() {
    if (!_names) return;
    tcpip_callback([](void* queue) {
        auto* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!ap) return;
        auto* iface = netif_get_by_index(esp_netif_get_netif_impl_index(ap));
        if (!iface) return;
        if (!nameListener) nameListener = raw_new_ip_type(IPADDR_TYPE_V4, IP_PROTO_UDP);
        if (!nameListener) return;
        ip_set_option(nameListener, SOF_BROADCAST);
        raw_bind_netif(nameListener, iface);
        raw_recv(nameListener, readDhcpName, queue);
    }, _names);
}

void ClientTracker::loop() {
    ClientHostname record;
    while (_names && xQueueReceive(_names, &record, 0) == pdTRUE) {
        ScopedLock lock(_mutex);
        auto it = _clients.find(macToString(record.mac));
        if (it != _clients.end()) it->second.hostname = record.name;
    }
    uint32_t now = millis();
    if ((uint32_t)(now - _lastPoll) < 2000UL) {
        return;
    }
    _lastPoll = now;
    poll();
}

void ClientTracker::onConnected(const String& mac) {
    String normalized = ConfigStore::normalizeMac(mac);
    if (normalized.length() != 17) {
        return;
    }

    ScopedLock lock(_mutex);
    Entry& entry = _clients[normalized];
    entry.mac = normalized;
    entry.online = true;
    entry.connectedAt = millis();
    entry.lastSeen = entry.connectedAt;
    entry.name = _store->clientName(normalized);
    entry.blocked = _store->isBlocked(normalized);

    if (entry.blocked) {
        kickBlockedLocked();
    }
}

void ClientTracker::onDisconnected(const String& mac) {
    String normalized = ConfigStore::normalizeMac(mac);
    ScopedLock lock(_mutex);
    auto it = _clients.find(normalized);
    if (it != _clients.end()) {
        it->second.online = false;
        it->second.lastSeen = millis();
    }
}

void ClientTracker::assignIp(const uint8_t* mac, uint32_t ip) {
    String normalized = macToString(mac);
    ScopedLock lock(_mutex);
    Entry& entry = _clients[normalized];
    entry.mac = normalized;
    entry.ip = ipToString(ip);
    entry.lastSeen = millis();
    if (entry.name.isEmpty() && _store) {
        entry.name = _store->clientName(normalized);
    }
}

void ClientTracker::poll() {
    if (!_store) {
        return;
    }

    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK) {
        return;
    }

    ScopedLock lock(_mutex);

    for (auto& pair : _clients) {
        pair.second.online = false;
    }

    uint32_t now = millis();
    for (int i = 0; i < staList.num; i++) {
        const wifi_sta_info_t& sta = staList.sta[i];
        String mac = macToString(sta.mac);

        Entry& entry = _clients[mac];
        entry.mac = mac;
        entry.rssi = sta.rssi;
        entry.lastSeen = now;
        if (!entry.online) {
            entry.online = true;
            entry.connectedAt = now;
        }
        entry.name = _store->clientName(mac);
        entry.blocked = _store->isBlocked(mac);
    }

    // Resolve IPs through the DHCP server's lease table.
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && staList.num > 0 && staList.num <= 10) {
        esp_netif_pair_mac_ip_t pairs[10] = {};
        for (int i = 0; i < staList.num; i++) {
            memcpy(pairs[i].mac, staList.sta[i].mac, 6);
            pairs[i].ip.addr = 0;
        }
        if (esp_netif_dhcps_get_clients_by_mac(ap, staList.num, pairs) == ESP_OK) {
            for (int i = 0; i < staList.num; i++) {
                if (pairs[i].ip.addr != 0) {
                    String mac = macToString(pairs[i].mac);
                    _clients[mac].ip = ipToString(pairs[i].ip.addr);
                }
            }
        }
    }

    // ARP fallback for clients that have not received/kept a DHCP lease.
#if LWIP_ARP
    ip4_addr_t* arpIp = nullptr;
    struct netif* arpNetif = nullptr;
    struct eth_addr* arpEth = nullptr;
    for (u8_t i = 0; etharp_get_entry(i, &arpIp, &arpNetif, &arpEth); i++) {
        if (!arpIp || !arpEth) {
            continue;
        }
        String mac = macToString(arpEth->addr);
        auto it = _clients.find(mac);
        if (it != _clients.end() && it->second.ip.isEmpty()) {
            it->second.ip = ipToString(arpIp->addr);
        }
    }
#endif

    kickBlockedLocked();
}

void ClientTracker::kickBlocked() {
    ScopedLock lock(_mutex);
    kickBlockedLocked();
}

void ClientTracker::kickBlockedLocked() {
    for (const auto& pair : _clients) {
        const Entry& entry = pair.second;
        if (!entry.online || !entry.blocked) {
            continue;
        }

        uint8_t mac[6];
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)strtoul(entry.mac.substring(i * 3, i * 3 + 2).c_str(), nullptr, 16);
        }

        uint16_t aid = 0;
        if (esp_wifi_ap_get_sta_aid(mac, &aid) == ESP_OK) {
            esp_wifi_deauth_sta(aid);
            log_i("Kicked blocked client %s", entry.mac.c_str());
        }
    }
}

String ClientTracker::json() {
    JsonDocument doc;
    JsonArray clients = doc["clients"].to<JsonArray>();
    uint32_t now = millis();

    ScopedLock lock(_mutex);
    for (const auto& pair : _clients) {
        const Entry& entry = pair.second;
        if (!entry.online && (uint32_t)(now - entry.lastSeen) > 300000UL) {
            continue;
        }

        JsonObject item = clients.add<JsonObject>();
        item["mac"] = entry.mac;
        item["ip"] = entry.ip;
        item["hostname"] = entry.hostname;
        item["name"] = entry.name;
        item["rssi"] = entry.rssi;
        item["blocked"] = entry.blocked;
        item["online"] = entry.online;
        item["connectedSec"] = entry.online ? (uint32_t)((now - entry.connectedAt) / 1000UL) : 0;
        item["lastSeenSec"] = (uint32_t)((now - entry.lastSeen) / 1000UL);
    }

    int online = 0;
    for (const auto& pair : _clients) {
        if (pair.second.online) {
            online++;
        }
    }
    doc["online"] = online;

    String out;
    serializeJson(doc, out);
    return out;
}

bool ClientTracker::block(const String& mac, bool blocked) {
    if (!_store || !_store->setBlocked(mac, blocked)) {
        return false;
    }

    String normalized = ConfigStore::normalizeMac(mac);
    ScopedLock lock(_mutex);
    auto it = _clients.find(normalized);
    if (it != _clients.end()) {
        it->second.blocked = blocked;
    }

    if (blocked) {
        kickBlockedLocked();
    }
    return true;
}

bool ClientTracker::rename(const String& mac, const String& name) {
    if (!_store) {
        return false;
    }

    _store->setClientName(mac, name);

    String normalized = ConfigStore::normalizeMac(mac);
    ScopedLock lock(_mutex);
    auto it = _clients.find(normalized);
    if (it != _clients.end()) {
        it->second.name = _store->clientName(normalized);
    }
    return true;
}

int ClientTracker::onlineCount() {
    ScopedLock lock(_mutex);
    int count = 0;
    for (const auto& pair : _clients) {
        if (pair.second.online) {
            count++;
        }
    }
    return count;
}

String ClientTracker::macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

String ClientTracker::ipToString(uint32_t ip) {
    esp_ip4_addr_t addr;
    addr.addr = ip;
    char buf[16];
    esp_ip4addr_ntoa(&addr, buf, sizeof(buf));
    return String(buf);
}

void ClientTracker::ipEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<ClientTracker*>(arg);
    auto* event = static_cast<ip_event_ap_staipassigned_t*>(data);
    if (self && event) {
        self->assignIp(event->mac, event->ip.addr);
    }
}
