/*
 * Copyright (C) 2022 Alibaba Group Holding Limited
 */
// Huge rework of this file was made by LLM, but verified & tested by human.
#include <stdbool.h>

#include <aos/kernel.h>
#include <aos/kv.h>
#include <ulog/ulog.h>

#include <drv/pwm.h>

#include "pwm_led.h"
#include "pwm_led_shows.h"

#define TAG "PWMLED"

#define PWM_ID                       (0)
#define USE_PWM_CHANNEL_RED          (3)
#define USE_PWM_CHANNEL_GREEN        (0)
#define USE_PWM_CHANNEL_BLUE         (1)

#define PWM_LED_COMMAND_QUEUE_DEPTH  (10U)
#define PWM_LED_PENDING_QUEUE_DEPTH  (10U)
#define PWM_LED_RGB_PERIOD           (255U)
#define PWM_LED_BRIGHTNESS_DIVISOR   (2U)
#define PWM_LED_TASK_STACK_SIZE      (4096U)
#define PWM_LED_DRAIN_SLICE_MS       (20U)

typedef enum {
    PWM_LED_COMMAND_PLAY_SHOW = 0,
    PWM_LED_COMMAND_SET_STATE,
    PWM_LED_COMMAND_CLEAR_STATE,
    PWM_LED_COMMAND_CLEAR_RGB
} pwm_led_command_type_t;

typedef struct {
    pwm_led_command_type_t type;
    light_show_state_types_t show_id;
    light_show_msg_flags_t flags;
} pwm_led_command_t;

typedef enum {
    PWM_LED_PENDING_SHOW = 0,
    PWM_LED_PENDING_BLACK
} pwm_led_pending_item_type_t;

typedef struct {
    pwm_led_pending_item_type_t type;
    light_show_state_types_t show_id;
} pwm_led_pending_item_t;

typedef struct {
    pwm_led_pending_item_t pending_items[PWM_LED_PENDING_QUEUE_DEPTH];
    uint8_t pending_head;
    uint8_t pending_tail;
    uint8_t pending_count;
    uint8_t has_interrupt_item;
    uint8_t has_state_show;
    uint8_t state_needs_apply;
    uint8_t has_active_transient;
    uint8_t active_transient_is_latched;
    light_show_state_types_t state_show_id;
    pwm_led_pending_item_t interrupt_item;
    pwm_led_pending_item_t active_transient;
    uint32_t state_generation;
    uint32_t active_transient_state_generation;
} pwm_led_scheduler_t;

typedef enum {
    PWM_LED_PLAY_RESULT_ERROR = -1,
    PWM_LED_PLAY_RESULT_OK = 0,
    PWM_LED_PLAY_RESULT_INTERRUPTED = 1
} pwm_led_play_result_t;

static csi_pwm_t g_pwm_r_handler;
static csi_pwm_t g_pwm_g_handler;
static csi_pwm_t g_pwm_b_handler;

static uint8_t s_q_buffer[sizeof(pwm_led_command_t) * PWM_LED_COMMAND_QUEUE_DEPTH];
static aos_queue_t s_queue;
static volatile light_show_state_types_t g_state_show_id = LIGHT_SHOW_NONE;

/* Whether the idle ("ready", waiting for wakeword) show is allowed to light
 * up the ring. Persisted so a user who wants a dark idle ring keeps that
 * choice across reboots; toggled via the MQTT "LED idle" switch. */
#define LED_IDLE_ENABLED_KV "led_idle_en"
static bool s_led_idle_enabled = true;

void led_idle_set_enabled(bool enabled)
{
    s_led_idle_enabled = enabled;
    aos_kv_setint(LED_IDLE_ENABLED_KV, enabled ? 1 : 0);

    /* Apply immediately instead of waiting for the next idle transition.
     * The idle glow is normally driven through light_show_state_msg_send()
     * (see wyoming.c), which uses the transient/pending-show queue, not the
     * separate "state" layer -- clear both so this works regardless of
     * which path last lit the ring. */
    if (enabled) {
        light_show_state_msg_send(LIGHT_SHOW_READY, NULL);
    } else {
        light_show_rgb_clear();
        light_show_state_clear();
    }
}

bool led_idle_get_enabled(void)
{
    return s_led_idle_enabled;
}

void led_idle_reload_from_kv(void)
{
    int led_idle_en = 1;
    aos_kv_getint(LED_IDLE_ENABLED_KV, &led_idle_en);
    s_led_idle_enabled = (led_idle_en != 0);
}

/*
 * Hex colors are authored in a perceptual space, while PWM duty is linear.
 * Applying a gamma curve here keeps dark colors visually dark on the strip.
 */
static const uint8_t s_pwm_led_gamma_table[256] = {
      0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   0U,   1U,
      1U,   1U,   1U,   1U,   1U,   1U,   1U,   1U,   1U,   2U,   2U,   2U,   2U,   2U,   2U,   2U,
      3U,   3U,   3U,   3U,   3U,   4U,   4U,   4U,   4U,   5U,   5U,   5U,   5U,   6U,   6U,   6U,
      6U,   7U,   7U,   7U,   8U,   8U,   8U,   9U,   9U,   9U,  10U,  10U,  11U,  11U,  11U,  12U,
     12U,  13U,  13U,  13U,  14U,  14U,  15U,  15U,  16U,  16U,  17U,  17U,  18U,  18U,  19U,  19U,
     20U,  20U,  21U,  22U,  22U,  23U,  23U,  24U,  25U,  25U,  26U,  26U,  27U,  28U,  28U,  29U,
     30U,  30U,  31U,  32U,  33U,  33U,  34U,  35U,  35U,  36U,  37U,  38U,  39U,  39U,  40U,  41U,
     42U,  43U,  43U,  44U,  45U,  46U,  47U,  48U,  49U,  49U,  50U,  51U,  52U,  53U,  54U,  55U,
     56U,  57U,  58U,  59U,  60U,  61U,  62U,  63U,  64U,  65U,  66U,  67U,  68U,  69U,  70U,  71U,
     73U,  74U,  75U,  76U,  77U,  78U,  79U,  81U,  82U,  83U,  84U,  85U,  87U,  88U,  89U,  90U,
     91U,  93U,  94U,  95U,  97U,  98U,  99U, 100U, 102U, 103U, 105U, 106U, 107U, 109U, 110U, 111U,
    113U, 114U, 116U, 117U, 119U, 120U, 121U, 123U, 124U, 126U, 127U, 129U, 130U, 132U, 133U, 135U,
    137U, 138U, 140U, 141U, 143U, 145U, 146U, 148U, 149U, 151U, 153U, 154U, 156U, 158U, 159U, 161U,
    163U, 165U, 166U, 168U, 170U, 172U, 173U, 175U, 177U, 179U, 181U, 182U, 184U, 186U, 188U, 190U,
    192U, 194U, 196U, 197U, 199U, 201U, 203U, 205U, 207U, 209U, 211U, 213U, 215U, 217U, 219U, 221U,
    223U, 225U, 227U, 229U, 231U, 234U, 236U, 238U, 240U, 242U, 244U, 246U, 248U, 251U, 253U, 255U,
};

static int pwm_led_is_valid_effect(int effect_id)
{
    return (effect_id >= 0) && (effect_id < LIGHT_SHOW_STATE_MAX);
}

static int pwm_led_is_playable_show(int effect_id)
{
    return pwm_led_is_valid_effect(effect_id) && (effect_id != LIGHT_SHOW_NONE);
}

static const pwm_led_show_t *pwm_led_get_show(light_show_state_types_t effect_id)
{
    const pwm_led_show_t *show;

    if (!pwm_led_is_valid_effect((int)effect_id)) {
        LOGE(TAG, "invalid effect id:%d", effect_id);
        return NULL;
    }

    show = &g_pwm_led_shows[effect_id];
    if ((show->frames == NULL) || (show->frame_count == 0U)) {
        LOGE(TAG, "effect %d has no frames", effect_id);
        return NULL;
    }

    return show;
}

static uint32_t pwm_led_show_total_duration_ms(const pwm_led_show_t *show)
{
    uint16_t frame_index;
    uint32_t total_duration_ms = 0U;

    if (show == NULL) {
        return 0U;
    }

    for (frame_index = 0U; frame_index < show->frame_count; ++frame_index) {
        total_duration_ms += show->frames[frame_index].duration_ms;
    }

    return total_duration_ms;
}

static int pwm_led_show_should_repeat(const pwm_led_show_t *show)
{
    if (show == NULL) {
        return 0;
    }

    return (show->loop != 0U) && (pwm_led_show_total_duration_ms(show) > 0U);
}

static int pwm_led_show_should_latch(const pwm_led_show_t *show)
{
    if (show == NULL) {
        return 0;
    }

    return (show->loop != 0U) && (pwm_led_show_total_duration_ms(show) == 0U);
}

static uint32_t pwm_led_channel_to_pulse_percent(uint8_t channel)
{
    uint32_t corrected_channel;

    corrected_channel = s_pwm_led_gamma_table[channel];
    corrected_channel /= PWM_LED_BRIGHTNESS_DIVISOR;

    return ((uint32_t)100U * corrected_channel + (PWM_LED_RGB_PERIOD / 2U)) / PWM_LED_RGB_PERIOD;
}

static int pwm_led_apply_frame(const pwm_led_frame_t *frame)
{
    if (frame == NULL) {
        return -1;
    }

    return led_pwm_rgb_config(PWM_LED_RGB_PERIOD, PWM_LED_RGB_PERIOD, PWM_LED_RGB_PERIOD,
                              pwm_led_channel_to_pulse_percent(frame->red),
                              pwm_led_channel_to_pulse_percent(frame->green),
                              pwm_led_channel_to_pulse_percent(frame->blue));
}

static int pwm_led_apply_black(void)
{
    return led_pwm_rgb_config(PWM_LED_RGB_PERIOD, PWM_LED_RGB_PERIOD, PWM_LED_RGB_PERIOD, 0U, 0U, 0U);
}

static void pwm_led_scheduler_init(pwm_led_scheduler_t *scheduler)
{
    if (scheduler == NULL) {
        return;
    }

    scheduler->pending_head = 0U;
    scheduler->pending_tail = 0U;
    scheduler->pending_count = 0U;
    scheduler->has_interrupt_item = 0U;
    scheduler->has_state_show = 0U;
    scheduler->state_needs_apply = 0U;
    scheduler->has_active_transient = 0U;
    scheduler->active_transient_is_latched = 0U;
    scheduler->state_show_id = LIGHT_SHOW_NONE;
    scheduler->interrupt_item.type = PWM_LED_PENDING_BLACK;
    scheduler->interrupt_item.show_id = LIGHT_SHOW_NONE;
    scheduler->active_transient.type = PWM_LED_PENDING_BLACK;
    scheduler->active_transient.show_id = LIGHT_SHOW_NONE;
    scheduler->state_generation = 0U;
    scheduler->active_transient_state_generation = 0U;
}

static int pwm_led_scheduler_has_interrupt_item(const pwm_led_scheduler_t *scheduler)
{
    return (scheduler != NULL) && (scheduler->has_interrupt_item != 0U);
}

static void pwm_led_scheduler_clear_latched_transient(pwm_led_scheduler_t *scheduler)
{
    if ((scheduler == NULL) || (scheduler->active_transient_is_latched == 0U)) {
        return;
    }

    scheduler->has_active_transient = 0U;
    scheduler->active_transient_is_latched = 0U;
}

static void pwm_led_scheduler_track_looping_transient(pwm_led_scheduler_t *scheduler,
                                                      const pwm_led_pending_item_t *item)
{
    const pwm_led_show_t *show;

    if ((scheduler == NULL) || (item == NULL) || (item->type != PWM_LED_PENDING_SHOW)) {
        return;
    }

    show = pwm_led_get_show(item->show_id);
    if (pwm_led_show_should_repeat(show) || pwm_led_show_should_latch(show)) {
        scheduler->has_active_transient = 1U;
        scheduler->active_transient_is_latched = 0U;
        scheduler->active_transient = *item;
        scheduler->active_transient_state_generation = scheduler->state_generation;
    }
}

static int pwm_led_pending_push(pwm_led_scheduler_t *scheduler, const pwm_led_pending_item_t *item)
{
    if ((scheduler == NULL) || (item == NULL)) {
        return -1;
    }

    if (scheduler->pending_count >= PWM_LED_PENDING_QUEUE_DEPTH) {
        LOGE(TAG, "pending queue full");
        return -1;
    }

    scheduler->pending_items[scheduler->pending_tail] = *item;
    scheduler->pending_tail = (uint8_t)((scheduler->pending_tail + 1U) % PWM_LED_PENDING_QUEUE_DEPTH);
    scheduler->pending_count++;

    return 0;
}

static int pwm_led_pending_pop(pwm_led_scheduler_t *scheduler, pwm_led_pending_item_t *item)
{
    if ((scheduler == NULL) || (item == NULL) || (scheduler->pending_count == 0U)) {
        return -1;
    }

    *item = scheduler->pending_items[scheduler->pending_head];
    scheduler->pending_head = (uint8_t)((scheduler->pending_head + 1U) % PWM_LED_PENDING_QUEUE_DEPTH);
    scheduler->pending_count--;

    return 0;
}

static int pwm_led_validate_command(const pwm_led_command_t *command, size_t len)
{
    if (len != sizeof(*command)) {
        LOGE(TAG, "unexpected command size:%u", (unsigned int)len);
        return -1;
    }

    if (command == NULL) {
        return -1;
    }

    switch (command->type) {
        case PWM_LED_COMMAND_PLAY_SHOW:
            if ((command->flags & ~LIGHT_SHOW_MSG_FLAG_INTERRUPT) != LIGHT_SHOW_MSG_FLAG_NONE) {
                LOGE(TAG, "invalid transient flags:%lu", (unsigned long)command->flags);
                return -1;
            }

            if (!pwm_led_is_playable_show((int)command->show_id)) {
                LOGE(TAG, "invalid transient effect id:%d", command->show_id);
                return -1;
            }
            break;
        case PWM_LED_COMMAND_SET_STATE:
            if (command->flags != LIGHT_SHOW_MSG_FLAG_NONE) {
                LOGE(TAG, "state command does not accept flags");
                return -1;
            }

            if (!pwm_led_is_valid_effect((int)command->show_id)) {
                LOGE(TAG, "invalid effect id:%d", command->show_id);
                return -1;
            }
            break;
        case PWM_LED_COMMAND_CLEAR_STATE:
        case PWM_LED_COMMAND_CLEAR_RGB:
            if (command->flags != LIGHT_SHOW_MSG_FLAG_NONE) {
                LOGE(TAG, "command %d does not accept flags", command->type);
                return -1;
            }
            break;
        default:
            LOGE(TAG, "invalid command type:%d", command->type);
            return -1;
    }

    return 0;
}

static void pwm_led_process_command(pwm_led_scheduler_t *scheduler, const pwm_led_command_t *command)
{
    pwm_led_pending_item_t item;

    if ((scheduler == NULL) || (command == NULL)) {
        return;
    }

    switch (command->type) {
        case PWM_LED_COMMAND_PLAY_SHOW:
            item.type = PWM_LED_PENDING_SHOW;
            item.show_id = command->show_id;

            pwm_led_scheduler_clear_latched_transient(scheduler);

            if ((command->flags & LIGHT_SHOW_MSG_FLAG_INTERRUPT) != 0U) {
                scheduler->has_interrupt_item = 1U;
                scheduler->interrupt_item = item;
                scheduler->has_active_transient = 0U;
                scheduler->active_transient_is_latched = 0U;
                if (scheduler->has_state_show) {
                    scheduler->state_needs_apply = 1U;
                }
                break;
            }

            (void)pwm_led_pending_push(scheduler, &item);
            break;
        case PWM_LED_COMMAND_SET_STATE:
            pwm_led_scheduler_clear_latched_transient(scheduler);
            if (command->show_id == LIGHT_SHOW_NONE) {
                scheduler->has_state_show = 0U;
                scheduler->state_show_id = LIGHT_SHOW_NONE;
                scheduler->state_needs_apply = 0U;
                break;
            }

            scheduler->has_state_show = 1U;
            scheduler->state_show_id = command->show_id;
            scheduler->state_needs_apply = 1U;
            scheduler->state_generation++;
            break;
        case PWM_LED_COMMAND_CLEAR_STATE:
            pwm_led_scheduler_clear_latched_transient(scheduler);
            scheduler->has_state_show = 0U;
            scheduler->state_show_id = LIGHT_SHOW_NONE;
            scheduler->state_needs_apply = 0U;
            break;
        case PWM_LED_COMMAND_CLEAR_RGB:
            pwm_led_scheduler_clear_latched_transient(scheduler);
            item.type = PWM_LED_PENDING_BLACK;
            item.show_id = LIGHT_SHOW_NONE;
            (void)pwm_led_pending_push(scheduler, &item);
            break;
        default:
            break;
    }
}

static int pwm_led_drain_commands(pwm_led_scheduler_t *scheduler, unsigned int timeout_ms)
{
    pwm_led_command_t command;
    size_t len;
    int processed = 0;
    int ret;

    if (scheduler == NULL) {
        return 0;
    }

    while (1) {
        len = 0U;
        ret = aos_queue_recv(&s_queue, timeout_ms, &command, &len);
        if (ret != 0) {
            break;
        }

        timeout_ms = 0U;
        if (pwm_led_validate_command(&command, len) != 0) {
            continue;
        }

        pwm_led_process_command(scheduler, &command);
        processed++;
    }

    return processed;
}

static int pwm_led_delay_and_drain(pwm_led_scheduler_t *scheduler, uint32_t duration_ms)
{
    uint32_t sleep_ms;

    while (duration_ms > 0U) {
        sleep_ms = duration_ms > PWM_LED_DRAIN_SLICE_MS ? PWM_LED_DRAIN_SLICE_MS : duration_ms;
        aos_msleep(sleep_ms);
        duration_ms -= sleep_ms;
        (void)pwm_led_drain_commands(scheduler, 0U);

        if (pwm_led_scheduler_has_interrupt_item(scheduler)) {
            return PWM_LED_PLAY_RESULT_INTERRUPTED;
        }
    }

    return PWM_LED_PLAY_RESULT_OK;
}

static int pwm_led_play_show_pass(light_show_state_types_t show_id, pwm_led_scheduler_t *scheduler)
{
    const pwm_led_show_t *show;
    uint16_t frame_index;
    int ret;

    show = pwm_led_get_show(show_id);
    if (show == NULL) {
        return PWM_LED_PLAY_RESULT_ERROR;
    }

    for (frame_index = 0U; frame_index < show->frame_count; ++frame_index) {
        if (pwm_led_scheduler_has_interrupt_item(scheduler)) {
            return PWM_LED_PLAY_RESULT_INTERRUPTED;
        }

        if (pwm_led_apply_frame(&show->frames[frame_index]) != 0) {
            LOGE(TAG, "failed to apply frame %u for effect %d", frame_index, show_id);
            return PWM_LED_PLAY_RESULT_ERROR;
        }

        if (show->frames[frame_index].duration_ms > 0U) {
            ret = pwm_led_delay_and_drain(scheduler, show->frames[frame_index].duration_ms);
            if (ret != PWM_LED_PLAY_RESULT_OK) {
                return ret;
            }
        } else {
            (void)pwm_led_drain_commands(scheduler, 0U);
            if (pwm_led_scheduler_has_interrupt_item(scheduler)) {
                return PWM_LED_PLAY_RESULT_INTERRUPTED;
            }
        }
    }

    return PWM_LED_PLAY_RESULT_OK;
}

static int pwm_led_play_pending_item(const pwm_led_pending_item_t *item, pwm_led_scheduler_t *scheduler)
{
    if ((item == NULL) || (scheduler == NULL)) {
        return -1;
    }

    if (item->type == PWM_LED_PENDING_BLACK) {
        if (pwm_led_apply_black() != 0) {
            LOGE(TAG, "failed to clear RGB output");
            return PWM_LED_PLAY_RESULT_ERROR;
        }

        (void)pwm_led_drain_commands(scheduler, 0U);
        return PWM_LED_PLAY_RESULT_OK;
    }

    return pwm_led_play_show_pass(item->show_id, scheduler);
}

static int pwm_led_scheduler_has_work(pwm_led_scheduler_t *scheduler)
{
    const pwm_led_show_t *show;

    if (scheduler == NULL) {
        return 0;
    }

    if (scheduler->has_interrupt_item || (scheduler->pending_count > 0U)) {
        return 1;
    }

    if (scheduler->has_active_transient) {
        return scheduler->active_transient_is_latched == 0U;
    }

    if (!scheduler->has_state_show) {
        return 0;
    }

    show = pwm_led_get_show(scheduler->state_show_id);
    if (show == NULL) {
        scheduler->has_state_show = 0U;
        scheduler->state_needs_apply = 0U;
        return 0;
    }

    return pwm_led_show_should_repeat(show) || (scheduler->state_needs_apply != 0U);
}

static int pwm_led_scheduler_select_next_item(pwm_led_scheduler_t *scheduler,
                                              pwm_led_pending_item_t *item,
                                              uint8_t *is_state_show,
                                              uint32_t *state_generation_snapshot)
{
    const pwm_led_show_t *show;

    if ((scheduler == NULL) || (item == NULL) || (is_state_show == NULL) || (state_generation_snapshot == NULL)) {
        return -1;
    }

    *is_state_show = 0U;
    *state_generation_snapshot = scheduler->state_generation;

    if (scheduler->has_interrupt_item) {
        *item = scheduler->interrupt_item;
        scheduler->has_interrupt_item = 0U;
        pwm_led_scheduler_track_looping_transient(scheduler, item);
        return 0;
    }

    if (scheduler->has_active_transient) {
        if (scheduler->active_transient_is_latched != 0U) {
            scheduler->has_active_transient = 0U;
            scheduler->active_transient_is_latched = 0U;
        } else {
            *item = scheduler->active_transient;
            return 0;
        }
    }

    if (pwm_led_pending_pop(scheduler, item) == 0) {
        pwm_led_scheduler_track_looping_transient(scheduler, item);
        return 0;
    }

    if (!scheduler->has_state_show) {
        return -1;
    }

    show = pwm_led_get_show(scheduler->state_show_id);
    if (show == NULL) {
        scheduler->has_state_show = 0U;
        scheduler->state_needs_apply = 0U;
        return -1;
    }

    if (!pwm_led_show_should_repeat(show) && (scheduler->state_needs_apply == 0U)) {
        return -1;
    }

    item->type = PWM_LED_PENDING_SHOW;
    item->show_id = scheduler->state_show_id;
    *is_state_show = 1U;
    *state_generation_snapshot = scheduler->state_generation;

    return 0;
}

static void pwm_led_scheduler_finish_transient(pwm_led_scheduler_t *scheduler, const pwm_led_pending_item_t *item)
{
    const pwm_led_show_t *show;

    if ((scheduler == NULL) || (item == NULL)) {
        return;
    }

    scheduler->has_active_transient = 0U;
    scheduler->active_transient_is_latched = 0U;

    if (item->type == PWM_LED_PENDING_SHOW) {
        show = pwm_led_get_show(item->show_id);
        if ((show != NULL) && (scheduler->pending_count == 0U) &&
            (scheduler->state_generation == scheduler->active_transient_state_generation)) {
            if (pwm_led_show_should_repeat(show)) {
                scheduler->has_active_transient = 1U;
            } else if (pwm_led_show_should_latch(show)) {
                scheduler->has_active_transient = 1U;
                scheduler->active_transient_is_latched = 1U;
            }
        }
    }

    if (!scheduler->has_active_transient && scheduler->has_state_show) {
        scheduler->state_needs_apply = 1U;
    }
}

static void pwm_led_scheduler_finish_state_show(pwm_led_scheduler_t *scheduler,
                                                const pwm_led_pending_item_t *item,
                                                uint32_t state_generation_snapshot)
{
    if ((scheduler == NULL) || (item == NULL)) {
        return;
    }

    if (!scheduler->has_state_show) {
        scheduler->state_needs_apply = 0U;
        return;
    }

    if ((scheduler->state_generation == state_generation_snapshot) &&
        (scheduler->state_show_id == item->show_id)) {
        scheduler->state_needs_apply = 0U;
    }
}

static void light_show_msg_task(void *arg)
{
    pwm_led_scheduler_t scheduler;
    pwm_led_pending_item_t item;
    uint8_t is_state_show;
    uint32_t state_generation_snapshot;

    (void)arg;

    pwm_led_scheduler_init(&scheduler);

    while (1) {
        while (!pwm_led_scheduler_has_work(&scheduler)) {
            (void)pwm_led_drain_commands(&scheduler, AOS_WAIT_FOREVER);
        }

        if (pwm_led_scheduler_select_next_item(&scheduler, &item, &is_state_show, &state_generation_snapshot) != 0) {
            continue;
        }

        {
            int play_result = pwm_led_play_pending_item(&item, &scheduler);

            if (play_result == PWM_LED_PLAY_RESULT_INTERRUPTED) {
                if (!is_state_show) {
                    scheduler.has_active_transient = 0U;
                    scheduler.active_transient_is_latched = 0U;
                    if (scheduler.has_state_show) {
                        scheduler.state_needs_apply = 1U;
                    }
                }
                continue;
            }

            if (play_result != PWM_LED_PLAY_RESULT_OK) {
                if (is_state_show) {
                    scheduler.has_state_show = 0U;
                    scheduler.state_needs_apply = 0U;
                } else {
                    scheduler.has_active_transient = 0U;
                    scheduler.active_transient_is_latched = 0U;
                    if (scheduler.has_state_show) {
                        scheduler.state_needs_apply = 1U;
                    }
                }
                continue;
            }
        }

        if (is_state_show) {
            pwm_led_scheduler_finish_state_show(&scheduler, &item, state_generation_snapshot);
        } else {
            pwm_led_scheduler_finish_transient(&scheduler, &item);
        }
    }
}

static int pwm_led_send_command(const pwm_led_command_t *command)
{
    pwm_led_command_t queue_command;
    int ret;

    if (command == NULL) {
        return -1;
    }

    if (pwm_led_validate_command(command, sizeof(*command)) != 0) {
        return -1;
    }

    queue_command = *command;
    ret = aos_queue_send(&s_queue, &queue_command, sizeof(queue_command));
    if (ret < 0) {
        LOGE(TAG, "queue send failed");
        return -1;
    }

    return 0;
}

int led_pwm_rgb_init(void)
{
    int ret = 0;

    ret |= csi_pwm_init(&g_pwm_r_handler, PWM_ID);
    ret |= csi_pwm_init(&g_pwm_g_handler, PWM_ID);
    ret |= csi_pwm_init(&g_pwm_b_handler, PWM_ID);

    if (ret != 0) {
        LOGE(TAG, "PWM init failed");
        return -1;
    }

    return 0;
}

int led_pwm_rgb_start(void)
{
    int ret = 0;

    ret |= csi_pwm_out_start(&g_pwm_b_handler, USE_PWM_CHANNEL_BLUE);
    ret |= csi_pwm_out_start(&g_pwm_g_handler, USE_PWM_CHANNEL_GREEN);
    ret |= csi_pwm_out_start(&g_pwm_r_handler, USE_PWM_CHANNEL_RED);

    if (ret != 0) {
        LOGE(TAG, "PWM start failed");
        return -1;
    }

    return 0;
}

int led_pwm_rgb_config(uint32_t r_period, uint32_t g_period, uint32_t b_period, uint32_t r_pulse, uint32_t g_pulse, uint32_t b_pulse)
{
    int ret = 0;
    uint32_t r_pulse_us = r_period * r_pulse / 100U;
    uint32_t g_pulse_us = g_period * g_pulse / 100U;
    uint32_t b_pulse_us = b_period * b_pulse / 100U;

    ret |= csi_pwm_out_config(&g_pwm_r_handler, USE_PWM_CHANNEL_RED, r_period, r_pulse_us, 0);
    ret |= csi_pwm_out_config(&g_pwm_g_handler, USE_PWM_CHANNEL_GREEN, g_period, g_pulse_us, 0);
    ret |= csi_pwm_out_config(&g_pwm_b_handler, USE_PWM_CHANNEL_BLUE, b_period, b_pulse_us, 0);

    if (ret != 0) {
        LOGE(TAG, "PWM config failed");
        return -1;
    }

    return 0;
}

void led_pwm_rgb_stop(void)
{
    csi_pwm_out_stop(&g_pwm_r_handler, USE_PWM_CHANNEL_RED);
    csi_pwm_out_stop(&g_pwm_g_handler, USE_PWM_CHANNEL_GREEN);
    csi_pwm_out_stop(&g_pwm_b_handler, USE_PWM_CHANNEL_BLUE);
}

void led_pwm_rgb_uninit(void)
{
    csi_pwm_uninit(&g_pwm_r_handler);
    csi_pwm_uninit(&g_pwm_g_handler);
    csi_pwm_uninit(&g_pwm_b_handler);
}

int light_show_state_init(void)
{
    aos_task_t task_msg;
    int ret;

    g_state_show_id = LIGHT_SHOW_NONE;

    /* KV isn't initialized yet this early in board_yoc_init() (aos_kv_init()
     * only runs later, in app_sys_init()), so this always reads the
     * not-yet-ready default. Real persisted state is loaded via
     * led_idle_reload_from_kv(), called once KV is actually up -- see
     * app_main.c. */
    led_idle_reload_from_kv();

    ret = led_pwm_rgb_init();
    if (ret != 0) {
        LOGE(TAG, "init light show error");
        return -1;
    }

    ret = pwm_led_apply_black();
    if (ret != 0) {
        led_pwm_rgb_uninit();
        return -1;
    }

    ret = led_pwm_rgb_start();
    if (ret != 0) {
        led_pwm_rgb_uninit();
        return -1;
    }

    ret = aos_queue_new(&s_queue, s_q_buffer,
                        PWM_LED_COMMAND_QUEUE_DEPTH * sizeof(pwm_led_command_t),
                        sizeof(pwm_led_command_t));
    if (ret != 0) {
        LOGE(TAG, "init light show queue error");
        led_pwm_rgb_stop();
        led_pwm_rgb_uninit();
        return -1;
    }

    ret = aos_task_new_ext(&task_msg, "light_show_msg_task", light_show_msg_task, NULL,
                           PWM_LED_TASK_STACK_SIZE, AOS_DEFAULT_APP_PRI);
    if (ret != 0) {
        LOGE(TAG, "create light show task failed");
        aos_queue_free(&s_queue);
        led_pwm_rgb_stop();
        led_pwm_rgb_uninit();
        return -1;
    }

    return 0;
}

int light_show_state_msg_send(light_show_state_types_t state_id, void *arg)
{
    pwm_led_command_t command;

    if (state_id == LIGHT_SHOW_READY && !s_led_idle_enabled) {
        /* LIGHT_SHOW_READY normally goes through the transient/pending-show
         * queue (below), not the separate "state" layer -- light_show_state_clear()
         * clears the wrong layer here and leaves the ready show visibly playing. */
        return light_show_rgb_clear();
    }

    command.type = PWM_LED_COMMAND_PLAY_SHOW;
    command.show_id = state_id;
    command.flags = (light_show_msg_flags_t)(uintptr_t)arg;

    return pwm_led_send_command(&command);
}

int light_show_state_set(light_show_state_types_t state_id)
{
    pwm_led_command_t command;
    int ret;

    if (state_id == LIGHT_SHOW_NONE ||
        (state_id == LIGHT_SHOW_READY && !s_led_idle_enabled)) {
        return light_show_state_clear();
    }

    command.type = PWM_LED_COMMAND_SET_STATE;
    command.show_id = state_id;
    command.flags = LIGHT_SHOW_MSG_FLAG_NONE;

    ret = pwm_led_send_command(&command);
    if (ret == 0) {
        g_state_show_id = state_id;
    }

    return ret;
}

light_show_state_types_t light_show_state_get(void)
{
    return g_state_show_id;
}

int light_show_state_clear(void)
{
    pwm_led_command_t command;
    int ret;

    command.type = PWM_LED_COMMAND_CLEAR_STATE;
    command.show_id = LIGHT_SHOW_NONE;
    command.flags = LIGHT_SHOW_MSG_FLAG_NONE;

    ret = pwm_led_send_command(&command);
    if (ret == 0) {
        g_state_show_id = LIGHT_SHOW_NONE;
    }

    return ret;
}

int light_show_rgb_clear(void)
{
    pwm_led_command_t command;

    command.type = PWM_LED_COMMAND_CLEAR_RGB;
    command.show_id = LIGHT_SHOW_NONE;
    command.flags = LIGHT_SHOW_MSG_FLAG_NONE;

    return pwm_led_send_command(&command);
}
