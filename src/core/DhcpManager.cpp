#include "core/DhcpManager.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>
#include <lwip/ip4_addr.h>
#include "dhcpserver/dhcpserver.h"

static const char* TAG = "dhcp";

void DhcpManager::begin(ConfigStore* store) {
    _store = store;
    apply();
}

void DhcpManager::loop() {
    if (!_applyPending) {
        return;
    }
    _applyPending = false;
    apply();
}

bool DhcpManager::apply() {
    if (!_store) {
        return false;
    }

    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) {
        log_e("AP netif not found");
        return false;
    }

    esp_netif_dhcps_stop(ap);

    const DhcpConfig& cfg = _store->cfg().dhcp;

    dhcps_lease_t lease;
    lease.enable = true;
    if (!ip4addr_aton(cfg.start.c_str(), &lease.start_ip) ||
        !ip4addr_aton(cfg.end.c_str(), &lease.end_ip)) {
        log_e("Invalid DHCP pool %s - %s", cfg.start.c_str(), cfg.end.c_str());
        esp_netif_dhcps_start(ap);
        return false;
    }

    esp_err_t err = esp_netif_dhcps_option(ap,
                                           ESP_NETIF_OP_SET,
                                           ESP_NETIF_REQUESTED_IP_ADDRESS,
                                           &lease,
                                           sizeof(lease));
    if (err != ESP_OK) {
        log_w("Failed to set DHCP pool: %s", esp_err_to_name(err));
    }

    uint32_t leaseSeconds = (uint32_t)cfg.leaseMinutes * 60UL;
    err = esp_netif_dhcps_option(ap,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                 &leaseSeconds,
                                 sizeof(leaseSeconds));
    if (err != ESP_OK) {
        log_w("Failed to set DHCP lease time: %s", esp_err_to_name(err));
    }

    // Offer the upstream DNS server to AP clients. Falls back to the router IP.
    esp_netif_dns_info_t dns = {};
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool haveDns = false;

    if (!cfg.captivePortal && !_store->setupRequired() && sta && WiFi.status() == WL_CONNECTED) {
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4 &&
            dns.ip.u_addr.ip4.addr != 0) {
            haveDns = true;
        }
    }

    if (!haveDns) {
        dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_ip_info_t apInfo;
        if (esp_netif_get_ip_info(ap, &apInfo) == ESP_OK) {
            dns.ip.u_addr.ip4 = apInfo.ip;
            haveDns = true;
        }
    }

    if (haveDns) {
        dhcps_offer_t offer = OFFER_DNS;
        esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
        esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
        char buf[16];
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, buf, sizeof(buf));
        _dns = buf;
    }

    err = esp_netif_dhcps_start(ap);
    if (err != ESP_OK) {
        log_e("Failed to start DHCP server: %s", esp_err_to_name(err));
        _applied = false;
        return false;
    }

    _applied = true;
    log_i("DHCP pool %s - %s, lease %d min, DNS %s",
          cfg.start.c_str(), cfg.end.c_str(), cfg.leaseMinutes, _dns.c_str());
    return true;
}

String DhcpManager::statusJson() {
    JsonDocument doc;
    doc["start"] = _store->cfg().dhcp.start;
    doc["end"] = _store->cfg().dhcp.end;
    doc["leaseMinutes"] = _store->cfg().dhcp.leaseMinutes;
    doc["dns"] = _dns;
    doc["applied"] = _applied;
    doc["captivePortal"] = _store->cfg().dhcp.captivePortal;

    String out;
    serializeJson(doc, out);
    return out;
}
