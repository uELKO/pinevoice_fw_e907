#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aos/kernel.h>
#include <aos/kv.h>
#include <smart_audio.h>
#include <ulog/ulog.h>
#include <wyoming/satellite.h>

#include "lwip/apps/mqtt.h"
#include "lwip/tcpip.h"

#include "mqtt_client.h"
#include "http_config.h"
#include "../display/pwm_led/pwm_led.h"
#include "../player/auto_volume.h"
#include "../sys/app_sys.h"
#include "wifi_mgmr_ext.h"

/* Sets the codec's hardware gain register (0~100) -- see app_key_msg.c for
 * why this needs to move in lockstep with smtaudio_vol_*. */
extern int volume2db2regval(int val);

#define TAG "MQTT"

#define MQTT_CHECK_DELAY_MS  2000
#define MQTT_RETRY_DELAY_MS  5000
#define MQTT_TOPIC_MAX       80
#define MQTT_PAYLOAD_MAX     640

enum mqtt_cmd_topic {
    MQTT_CMD_TOPIC_NONE,
    MQTT_CMD_TOPIC_RESTART,
    MQTT_CMD_TOPIC_LED_IDLE,
    MQTT_CMD_TOPIC_VOLUME,
    MQTT_CMD_TOPIC_VOL_MIN,
    MQTT_CMD_TOPIC_VOL_MAX,
};

static char s_mac_id[13]; /* "aabbccddeeff" */
static char s_mqtt_user[64];
static char s_mqtt_pass[64];
static mqtt_client_t *s_mqtt_client;
static enum mqtt_cmd_topic s_incoming_topic;

static void mqtt_pub(const char *topic, const char *payload, u16_t len, u8_t retain)
{
    err_t err = mqtt_publish(s_mqtt_client, topic, payload, len, 0, retain, NULL, NULL);
    if (err != ERR_OK) {
        LOGW(TAG, "mqtt_publish(%s) failed: %d", topic, err);
    }
}

static void mqtt_publish_led_idle_state(bool enabled)
{
    char topic[MQTT_TOPIC_MAX];

    snprintf(topic, sizeof(topic), "pinevoice/%s/led_idle/state", s_mac_id);
    mqtt_pub(topic, enabled ? "ON" : "OFF", enabled ? 2 : 3, 1);
}

static void mqtt_publish_volume_state(void)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[8];

    snprintf(topic, sizeof(topic), "pinevoice/%s/volume/state", s_mac_id);
    snprintf(payload, sizeof(payload), "%d", smtaudio_vol_get());
    mqtt_pub(topic, payload, strlen(payload), 1);
}

void mqtt_client_notify_volume_changed(void)
{
    mqtt_publish_volume_state();
}

static void mqtt_publish_auto_vol_state(void)
{
    int min_pct, max_pct;
    char topic[MQTT_TOPIC_MAX];
    char payload[8];

    auto_volume_get_range(&min_pct, &max_pct);

    snprintf(topic, sizeof(topic), "pinevoice/%s/auto_vol_min/state", s_mac_id);
    snprintf(payload, sizeof(payload), "%d", min_pct);
    mqtt_pub(topic, payload, strlen(payload), 1);

    snprintf(topic, sizeof(topic), "pinevoice/%s/auto_vol_max/state", s_mac_id);
    snprintf(payload, sizeof(payload), "%d", max_pct);
    mqtt_pub(topic, payload, strlen(payload), 1);
}

static void mqtt_publish_discovery(void)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[MQTT_PAYLOAD_MAX];
    char device_name[24];
    char device_block[200];

    snprintf(device_name, sizeof(device_name), "PineVoice-%s", s_mac_id + 6);
    snprintf(device_block, sizeof(device_block),
             "\"device\":{\"identifiers\":[\"pinevoice_%s\"],\"name\":\"%s\",\"model\":\"PineVoice\",\"manufacturer\":\"Pine64\"}",
             s_mac_id, device_name);

    snprintf(topic, sizeof(topic), "homeassistant/button/pinevoice_%s/restart/config", s_mac_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Restart\",\"unique_id\":\"pinevoice_%s_restart\","
             "\"command_topic\":\"pinevoice/%s/restart/set\","
             "\"availability_topic\":\"pinevoice/%s/status\",%s}",
             s_mac_id, s_mac_id, s_mac_id, device_block);
    mqtt_pub(topic, payload, strlen(payload), 1);

    snprintf(topic, sizeof(topic), "homeassistant/switch/pinevoice_%s/led_idle/config", s_mac_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"LED Idle\",\"unique_id\":\"pinevoice_%s_led_idle\","
             "\"command_topic\":\"pinevoice/%s/led_idle/set\","
             "\"state_topic\":\"pinevoice/%s/led_idle/state\","
             "\"availability_topic\":\"pinevoice/%s/status\",%s}",
             s_mac_id, s_mac_id, s_mac_id, s_mac_id, device_block);
    mqtt_pub(topic, payload, strlen(payload), 1);

    snprintf(topic, sizeof(topic), "homeassistant/number/pinevoice_%s/volume/config", s_mac_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Volume\",\"unique_id\":\"pinevoice_%s_volume\","
             "\"command_topic\":\"pinevoice/%s/volume/set\","
             "\"state_topic\":\"pinevoice/%s/volume/state\","
             "\"availability_topic\":\"pinevoice/%s/status\","
             "\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",%s}",
             s_mac_id, s_mac_id, s_mac_id, s_mac_id, device_block);
    mqtt_pub(topic, payload, strlen(payload), 1);

    snprintf(topic, sizeof(topic), "homeassistant/number/pinevoice_%s/auto_vol_min/config", s_mac_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Auto Volume Min %%\",\"unique_id\":\"pinevoice_%s_auto_vol_min\","
             "\"command_topic\":\"pinevoice/%s/auto_vol_min/set\","
             "\"state_topic\":\"pinevoice/%s/auto_vol_min/state\","
             "\"availability_topic\":\"pinevoice/%s/status\","
             "\"min\":50,\"max\":100,\"step\":5,%s}",
             s_mac_id, s_mac_id, s_mac_id, s_mac_id, device_block);
    mqtt_pub(topic, payload, strlen(payload), 1);

    snprintf(topic, sizeof(topic), "homeassistant/number/pinevoice_%s/auto_vol_max/config", s_mac_id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Auto Volume Max %%\",\"unique_id\":\"pinevoice_%s_auto_vol_max\","
             "\"command_topic\":\"pinevoice/%s/auto_vol_max/set\","
             "\"state_topic\":\"pinevoice/%s/auto_vol_max/state\","
             "\"availability_topic\":\"pinevoice/%s/status\","
             "\"min\":100,\"max\":200,\"step\":5,%s}",
             s_mac_id, s_mac_id, s_mac_id, s_mac_id, device_block);
    mqtt_pub(topic, payload, strlen(payload), 1);
}

static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    (void)arg;
    (void)tot_len;

    if (strstr(topic, "/restart/set")) {
        s_incoming_topic = MQTT_CMD_TOPIC_RESTART;
    } else if (strstr(topic, "/led_idle/set")) {
        s_incoming_topic = MQTT_CMD_TOPIC_LED_IDLE;
    } else if (strstr(topic, "/volume/set")) {
        s_incoming_topic = MQTT_CMD_TOPIC_VOLUME;
    } else if (strstr(topic, "/auto_vol_min/set")) {
        s_incoming_topic = MQTT_CMD_TOPIC_VOL_MIN;
    } else if (strstr(topic, "/auto_vol_max/set")) {
        s_incoming_topic = MQTT_CMD_TOPIC_VOL_MAX;
    } else {
        s_incoming_topic = MQTT_CMD_TOPIC_NONE;
    }
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    char payload[16];
    size_t n = len < sizeof(payload) - 1 ? len : sizeof(payload) - 1;
    int min_pct, max_pct;

    (void)arg;
    (void)flags;

    memcpy(payload, data, n);
    payload[n] = 0;

    switch (s_incoming_topic) {
    case MQTT_CMD_TOPIC_RESTART:
        app_sys_reboot(BOOT_REASON_SOFT_RESET);
        break;
    case MQTT_CMD_TOPIC_LED_IDLE:
        led_idle_set_enabled(strcmp(payload, "ON") == 0);
        mqtt_publish_led_idle_state(led_idle_get_enabled());
        break;
    case MQTT_CMD_TOPIC_VOLUME:
        smtaudio_vol_set(atoi(payload));
        volume2db2regval(smtaudio_vol_get());
        auto_volume_notify_manual_change();
        mqtt_publish_volume_state();
        break;
    case MQTT_CMD_TOPIC_VOL_MIN:
        auto_volume_get_range(&min_pct, &max_pct);
        auto_volume_set_range(atoi(payload), max_pct);
        mqtt_publish_auto_vol_state();
        break;
    case MQTT_CMD_TOPIC_VOL_MAX:
        auto_volume_get_range(&min_pct, &max_pct);
        auto_volume_set_range(min_pct, atoi(payload));
        mqtt_publish_auto_vol_state();
        break;
    default:
        break;
    }
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    char topic[MQTT_TOPIC_MAX];

    (void)arg;

    if (status != MQTT_CONNECT_ACCEPTED) {
        LOGW(TAG, "MQTT connect failed/dropped: %d", status);
        return;
    }

    LOGI(TAG, "MQTT connected");
    mqtt_set_inpub_callback(client, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, NULL);

    snprintf(topic, sizeof(topic), "pinevoice/%s/restart/set", s_mac_id);
    mqtt_subscribe(client, topic, 0, NULL, NULL);
    snprintf(topic, sizeof(topic), "pinevoice/%s/led_idle/set", s_mac_id);
    mqtt_subscribe(client, topic, 0, NULL, NULL);
    snprintf(topic, sizeof(topic), "pinevoice/%s/volume/set", s_mac_id);
    mqtt_subscribe(client, topic, 0, NULL, NULL);
    snprintf(topic, sizeof(topic), "pinevoice/%s/auto_vol_min/set", s_mac_id);
    mqtt_subscribe(client, topic, 0, NULL, NULL);
    snprintf(topic, sizeof(topic), "pinevoice/%s/auto_vol_max/set", s_mac_id);
    mqtt_subscribe(client, topic, 0, NULL, NULL);

    snprintf(topic, sizeof(topic), "pinevoice/%s/status", s_mac_id);
    mqtt_pub(topic, "online", 6, 1);

    mqtt_publish_discovery();
    mqtt_publish_led_idle_state(led_idle_get_enabled());
    mqtt_publish_volume_state();
    mqtt_publish_auto_vol_state();
}

static void mqtt_client_task(void *arg)
{
    uint8_t mac[6];

    (void)arg;

    bl_efuse_read_mac_smart(1, mac, 0);
    snprintf(s_mac_id, sizeof(s_mac_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    while (1) {
        if (aos_kv_getstring(MQTT_USER_KV, s_mqtt_user, sizeof(s_mqtt_user)) <= 0 ||
            aos_kv_getstring(MQTT_PASS_KV, s_mqtt_pass, sizeof(s_mqtt_pass)) <= 0) {
            aos_msleep(MQTT_CHECK_DELAY_MS);
            continue;
        }

        uint32_t peer = wsat_server_get_peer_addr();
        if (peer == 0) {
            aos_msleep(MQTT_CHECK_DELAY_MS);
            continue;
        }

        ip_addr_t broker_ip;
        ip_addr_set_ip4_u32(&broker_ip, peer);

        char client_id[24];
        snprintf(client_id, sizeof(client_id), "pinevoice-%s", s_mac_id);

        char status_topic[MQTT_TOPIC_MAX];
        snprintf(status_topic, sizeof(status_topic), "pinevoice/%s/status", s_mac_id);

        struct mqtt_connect_client_info_t client_info;
        memset(&client_info, 0, sizeof(client_info));
        client_info.client_id = client_id;
        client_info.client_user = s_mqtt_user;
        client_info.client_pass = s_mqtt_pass;
        client_info.keep_alive = 30;
        client_info.will_topic = status_topic;
        client_info.will_msg = "offline";
        client_info.will_qos = 0;
        client_info.will_retain = 1;

        if (s_mqtt_client == NULL) {
            s_mqtt_client = mqtt_client_new();
        }

        LOCK_TCPIP_CORE();
        err_t err = mqtt_client_connect(s_mqtt_client, &broker_ip, MQTT_PORT, mqtt_connection_cb, NULL, &client_info);
        UNLOCK_TCPIP_CORE();

        if (err != ERR_OK) {
            LOGW(TAG, "mqtt_client_connect() failed: %d", err);
            aos_msleep(MQTT_RETRY_DELAY_MS);
            continue;
        }

        u8_t connected;
        do {
            aos_msleep(MQTT_RETRY_DELAY_MS);
            LOCK_TCPIP_CORE();
            connected = mqtt_client_is_connected(s_mqtt_client);
            UNLOCK_TCPIP_CORE();
        } while (connected);
    }
}

void mqtt_client_start(void)
{
    aos_task_t task_handle;
    aos_task_new_ext(&task_handle, "mqtt_client", mqtt_client_task, NULL, 4096, AOS_DEFAULT_APP_PRI);
}
