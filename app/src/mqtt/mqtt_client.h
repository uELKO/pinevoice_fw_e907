#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

/* Starts the background task that connects to the MQTT broker (once
 * credentials are set via the web form and a Wyoming/HA peer is known)
 * and publishes the Home Assistant MQTT Discovery card: restart button,
 * LED idle switch, volume slider, auto-volume range. */
void mqtt_client_start(void);

/* Call after the volume changes through a non-MQTT path (e.g. physical
 * buttons) so the HA volume slider stays in sync. No-op if MQTT isn't
 * connected yet. */
void mqtt_client_notify_volume_changed(void);

#endif /* MQTT_CLIENT_H */
