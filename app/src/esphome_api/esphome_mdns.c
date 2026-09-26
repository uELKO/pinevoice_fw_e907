// Copyright 2026
// SPDX-License-Identifier: Apache-2.0
//
// mDNS advertisement for the ESPHome native API server, so Home Assistant's
// zeroconf discovery finds it as an "esphome" device
// (service type verified against home-assistant/core's esphome/manifest.json
// -- "zeroconf": ["_esphomelib._tcp.local."]).
//
// Deliberately parallels app/src/wyoming/mdns_advertiser.c rather than
// generalizing the two into one helper -- see TODO.md ("ESPHome-Native-API
// statt Wyoming"): the Wyoming service keeps running side by side during
// this migration, so touching its mDNS code isn't worth the risk right now.

#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include <stdio.h>
#include <string.h>
#include <bl_efuse.h>
#include <ulog/ulog.h>
#include "wifi_mgmr_ext.h"

static char s_mac_txt[18]; // "mac=aabbccddeeff" + NUL

static void esphome_srv_txt(struct mdns_service *service, void *txt_userdata)
{
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, s_mac_txt, (uint8_t)strlen(s_mac_txt));
}

static uint8_t s_mdns_started;
static uint8_t s_sta_added;
static char s_service_instance[32];

static int esphome_netif_is_registered(struct netif *target)
{
    struct netif *netif;
    for (netif = netif_list; netif != NULL; netif = netif->next) {
        if (netif == target) {
            return 1;
        }
    }
    return 0;
}

void esphome_mdns_advertise_start(const char *hostname)
{
    uint8_t mac[6];
    struct netif *sta_netif;

    if (s_mdns_started) {
        return;
    }

    bl_efuse_read_mac_smart(1, mac, 0);
    snprintf(s_mac_txt, sizeof(s_mac_txt), "mac=%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_service_instance, sizeof(s_service_instance), "%s", hostname);

    LOCK_TCPIP_CORE();

    // Wyoming's mdns_advertiser.c already calls mdns_resp_init() and adds
    // the STA netif under its own hostname -- lwIP's mDNS responder only
    // needs mdns_resp_add_netif() once per netif, and both services can be
    // registered on the same netif entry. If this ever runs standalone
    // (Wyoming removed), mdns_resp_init()/add_netif() would need to move
    // here.
    sta_netif = wifi_mgmr_sta_netif_get();
    if (sta_netif != NULL && esphome_netif_is_registered(sta_netif)) {
        err_t res = mdns_resp_add_service(sta_netif, s_service_instance, "_esphomelib",
                                           DNSSD_PROTO_TCP, 6053, 120 /*2 mins*/, esphome_srv_txt, NULL);
        if (res < 0) {
            LOGE("esphome_mdns", "mdns_resp_add_service failed: %d", res);
        } else {
            s_sta_added = 1;
        }
    }

    UNLOCK_TCPIP_CORE();

    s_mdns_started = 1;
}
