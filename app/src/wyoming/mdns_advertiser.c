#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include <aos/cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_mgmr_ext.h"

/*
  Source file by Ben Brown <ralim@ralimtek.com>
  Licensed under MIT licence
*/

static void srv_txt(struct mdns_service *service, void *txt_userdata) {
  //   err_t res;
  //   LWIP_UNUSED_ARG(txt_userdata);

  //   res = mdns_resp_add_service_txtitem(service, "path=/", 6);
  //   LWIP_ERROR("mdns add service txt failed\n", (res == ERR_OK), return);
}

static void wyoming_mdns_report(struct netif *netif, u8_t result) { LWIP_PLATFORM_DIAG(("mdns status[netif %d]: %d\n", netif->num, result)); }

static uint8_t s_mdns_running;
static uint8_t s_mdns_started;
static uint8_t s_mdns_sta_added;
static char s_service_instance[(6 * 2) + 1];
/* MAC-suffixed so multiple PineVoice units on the same network don't fight
 * over the same mDNS hostname -- lwIP's mDNS conflict "resolution" only
 * logs a warning, it doesn't actually rename on collision. */
static char s_hostname[10 + (3 * 2) + 1] = "pinevoice";

#if LWIP_NETIF_EXT_STATUS_CALLBACK
NETIF_DECLARE_EXT_CALLBACK(s_mdns_netif_cb);
static uint8_t s_mdns_cb_registered;
#endif

static int wyoming_is_sta_netif(struct netif *netif) {
  return netif == wifi_mgmr_sta_netif_get();
}

static int wyoming_netif_is_registered(struct netif *target) {
  struct netif *netif;
  for (netif = netif_list; netif != NULL; netif = netif->next) {
    if (netif == target) {
      return 1;
    }
  }
  return 0;
}

static void wyoming_mdns_attach_sta_locked(struct netif *sta_netif) {
  err_t res;

  if (sta_netif == NULL || s_mdns_sta_added) {
    return;
  }

  res = mdns_resp_add_netif(sta_netif, s_hostname, 120 /*2 mins*/);
  if (res != ERR_OK) {
    LOGE("mdns", "mdns_resp_add_netif failed: %d", res);
    return;
  }

  res = mdns_resp_add_service(sta_netif, s_service_instance, "_wyoming", DNSSD_PROTO_TCP, 10700, 120 /*2 mins*/, srv_txt, NULL);
  if (res < 0) {
    LOGE("mdns", "mdns_resp_add_service failed: %d", res);
    mdns_resp_remove_netif(sta_netif);
    return;
  }

  s_mdns_sta_added = 1;
}

static void wyoming_mdns_detach_sta_locked(struct netif *sta_netif) {
  if (sta_netif == NULL || !s_mdns_sta_added) {
    return;
  }

  mdns_resp_remove_netif(sta_netif);
  s_mdns_sta_added = 0;
}

#if LWIP_NETIF_EXT_STATUS_CALLBACK
static void wyoming_mdns_netif_cb(struct netif *netif, netif_nsc_reason_t reason, const netif_ext_callback_args_t *args) {
  LWIP_UNUSED_ARG(args);

  if (!s_mdns_running || !wyoming_is_sta_netif(netif)) {
    return;
  }

  if (reason & LWIP_NSC_NETIF_ADDED) {
    wyoming_mdns_attach_sta_locked(netif);
  }

  if (reason & LWIP_NSC_NETIF_REMOVED) {
    wyoming_mdns_detach_sta_locked(netif);
  }
}
#endif

void wyoming_mdns_advertise_start(void) {
  uint8_t mac[6];
  struct netif *sta_netif;

  if (s_mdns_running) {
    return;
  }

  bl_efuse_read_mac_smart(1, mac, 0);
  snprintf(s_service_instance, sizeof(s_service_instance), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(s_hostname, sizeof(s_hostname), "pinevoice-%02x%02x%02x", mac[3], mac[4], mac[5]);

  LOCK_TCPIP_CORE();

  if (!s_mdns_started) {
    mdns_resp_register_name_result_cb(wyoming_mdns_report);
    mdns_resp_init();
    s_mdns_started = 1;
  }

#if LWIP_NETIF_EXT_STATUS_CALLBACK
  if (!s_mdns_cb_registered) {
    netif_add_ext_callback(&s_mdns_netif_cb, wyoming_mdns_netif_cb);
    s_mdns_cb_registered = 1;
  }
#endif

  s_mdns_running = 1;

  sta_netif = wifi_mgmr_sta_netif_get();
  if (wyoming_netif_is_registered(sta_netif)) {
    wyoming_mdns_attach_sta_locked(sta_netif);
  }

  UNLOCK_TCPIP_CORE();
}

static void cmd_mdns(char *wbuf, int wbuf_len, int argc, char **argv) {
  LWIP_UNUSED_ARG(wbuf);
  LWIP_UNUSED_ARG(wbuf_len);
  LWIP_UNUSED_ARG(argc);
  LWIP_UNUSED_ARG(argv);

  LOGI("mdns", "Starting mDNS advertising");
  wyoming_mdns_advertise_start();
  printf("Started mDNS advertising\r\n");
}

void cli_reg_cmd_mdns(void) {
  static const struct cli_command cmd_info = {"mdns", "start mDNS", cmd_mdns};

  aos_cli_register_command(&cmd_info);
}
