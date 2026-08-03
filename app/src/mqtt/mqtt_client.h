#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

/* Starts the background task that connects to the MQTT broker (once
 * credentials are set via the web form and a Wyoming/HA peer is known)
 * and publishes the Home Assistant MQTT Discovery card: restart button,
 * LED idle switch, auto-volume range. */
void mqtt_client_start(void);

#endif /* MQTT_CLIENT_H */
