#ifndef HTTP_CONFIG_H
#define HTTP_CONFIG_H

#define MQTT_USER_KV "mqtt_user"
#define MQTT_PASS_KV "mqtt_pass"

/* Starts the local web server used to enter MQTT broker credentials,
 * reachable at http://<mdns hostname>/ once on the same network. */
void http_config_start(void);

#endif /* HTTP_CONFIG_H */
