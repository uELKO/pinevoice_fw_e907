/*
 * Copyright (C) 2022 Alibaba Group Holding Limited
 */

#ifndef _PWM_LED_H_
#define _PWM_LED_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LIGHT_SHOW_NONE,
    LIGHT_SHOW_PROVISIONING,
    LIGHT_SHOW_ERROR,
    LIGHT_SHOW_RGB_TEST,
    LIGHT_SHOW_STARTUP,
    LIGHT_SHOW_IDENTIFY,
    LIGHT_SHOW_WIFI_CONNECTING,
    LIGHT_SHOW_SAT_CONN_PENDING,
    LIGHT_SHOW_READY,
    LIGHT_SHOW_PROCESSING,
    LIGHT_SHOW_ANSWER,
    LIGHT_SHOW_LISTENING,
    LIGHT_SHOW_VOLUME_UP,
    LIGHT_SHOW_VOLUME_DOWN,
    LIGHT_SHOW_STATE_MAX
} light_show_state_types_t;

typedef uintptr_t light_show_msg_flags_t;

/* Encode message flags for light_show_state_msg_send(..., arg). NULL means no flags. */
#define LIGHT_SHOW_MSG_FLAG_NONE       ((light_show_msg_flags_t)0U)
#define LIGHT_SHOW_MSG_FLAG_INTERRUPT  (((light_show_msg_flags_t)1U) << 0)
#define LIGHT_SHOW_MSG_FLAGS(flags)    ((void *)(uintptr_t)(flags))

int led_pwm_rgb_init(void);
int led_pwm_rgb_start(void);
int led_pwm_rgb_config(uint32_t r_period, uint32_t g_period, uint32_t b_period, uint32_t r_pulse, uint32_t g_pulse, uint32_t b_pulse);
void led_pwm_rgb_stop(void);
void led_pwm_rgb_uninit(void);

int light_show_state_init(void);
int light_show_state_msg_send(light_show_state_types_t state_id, void *arg);
int light_show_state_set(light_show_state_types_t state_id);
light_show_state_types_t light_show_state_get(void);
int light_show_state_clear(void);
int light_show_rgb_clear(void);

/* Whether the idle/"ready" show is allowed to light up the ring. */
void led_idle_set_enabled(bool enabled);
bool led_idle_get_enabled(void);

/* Re-reads the persisted enabled state from KV. Call once KV is actually
 * initialized (see app_main.c) -- light_show_state_init() runs too early
 * for its own KV read to see real data. */
void led_idle_reload_from_kv(void);

#endif
