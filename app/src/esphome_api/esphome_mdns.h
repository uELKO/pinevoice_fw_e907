#ifndef APP_ESPHOME_API_MDNS_H_
#define APP_ESPHOME_API_MDNS_H_

// Must be called after wyoming_mdns_advertise_start() (see esphome_mdns.c
// for why -- phase 1 deliberately piggybacks on Wyoming's mDNS init/netif
// registration instead of duplicating it).
void esphome_mdns_advertise_start(const char *hostname);

#endif
