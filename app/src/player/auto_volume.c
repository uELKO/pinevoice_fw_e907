#include <aos/kernel.h>
#include <aos/kv.h>
#include <smart_audio.h>

#include "auto_volume.h"

/* Bounded so ambient-driven adjustments can never drown out or fall below
 * a sane range of the user's actual saved volume. Runtime-configurable
 * (e.g. via the MQTT card), persisted to KV. */
#define AUTO_VOL_MIN_PCT_KV  "auto_vol_min_pct"
#define AUTO_VOL_MAX_PCT_KV  "auto_vol_max_pct"
#define AUTO_VOL_MIN_PCT_DEFAULT  75
#define AUTO_VOL_MAX_PCT_DEFAULT  125
#define AUTO_VOL_PCT_FLOOR   50
#define AUTO_VOL_PCT_CEIL    200

/* How long to suppress ambient-driven adjustments after a manual volume
 * change, so a burst of button presses doesn't fight with an ambient report
 * landing in between. */
#define AUTO_VOL_MANUAL_COOLDOWN_MS  8000

/* Ambient envelope calibration (raw int16 RMS-ish scale, see agc.c on the
 * C906 side) -- tune against real hardware/room levels. */
#define AMBIENT_QUIET_LEVEL   200.0f
#define AMBIENT_LOUD_LEVEL   3000.0f

static float s_min_mult = AUTO_VOL_MIN_PCT_DEFAULT / 100.0f;
static float s_max_mult = AUTO_VOL_MAX_PCT_DEFAULT / 100.0f;
static long long s_last_manual_change_ms = 0;

void auto_volume_notify_manual_change(void)
{
    s_last_manual_change_ms = aos_now_ms();
}

void auto_volume_init(void)
{
    int min_pct = AUTO_VOL_MIN_PCT_DEFAULT;
    int max_pct = AUTO_VOL_MAX_PCT_DEFAULT;

    aos_kv_getint(AUTO_VOL_MIN_PCT_KV, &min_pct);
    aos_kv_getint(AUTO_VOL_MAX_PCT_KV, &max_pct);

    s_min_mult = min_pct / 100.0f;
    s_max_mult = max_pct / 100.0f;
}

void auto_volume_set_range(int min_pct, int max_pct)
{
    if (min_pct < AUTO_VOL_PCT_FLOOR) {
        min_pct = AUTO_VOL_PCT_FLOOR;
    }
    if (max_pct > AUTO_VOL_PCT_CEIL) {
        max_pct = AUTO_VOL_PCT_CEIL;
    }
    if (min_pct > max_pct) {
        min_pct = max_pct;
    }

    s_min_mult = min_pct / 100.0f;
    s_max_mult = max_pct / 100.0f;

    aos_kv_setint(AUTO_VOL_MIN_PCT_KV, min_pct);
    aos_kv_setint(AUTO_VOL_MAX_PCT_KV, max_pct);
}

void auto_volume_get_range(int *min_pct, int *max_pct)
{
    *min_pct = (int)(s_min_mult * 100.0f + 0.5f);
    *max_pct = (int)(s_max_mult * 100.0f + 0.5f);
}

static float level_to_multiplier(float level)
{
    if (level <= AMBIENT_QUIET_LEVEL) {
        return s_min_mult;
    }
    if (level >= AMBIENT_LOUD_LEVEL) {
        return s_max_mult;
    }

    float t = (level - AMBIENT_QUIET_LEVEL) / (AMBIENT_LOUD_LEVEL - AMBIENT_QUIET_LEVEL);
    return s_min_mult + t * (s_max_mult - s_min_mult);
}

void auto_volume_on_ambient_level(float level)
{
    /* Don't nudge the volume mid-playback; wait for the next report once
     * idle again so the change isn't audible as a jump. */
    if (smtaudio_get_state() == SMTAUDIO_STATE_PLAYING) {
        return;
    }

    /* Don't fight a manual change that just happened -- aui_player_vol_set()
     * below would otherwise land between two button presses and get read
     * back as the new "current" volume by the next one (see
     * local_play_vol_up/down in smart_audio), making it jump around. */
    if (aos_now_ms() - s_last_manual_change_ms < AUTO_VOL_MANUAL_COOLDOWN_MS) {
        return;
    }

    int base_vol = SMART_AUDIO_DEFAULT_VOLUME;
    aos_kv_getint(VOLUME_SAVE_KV_NAME, &base_vol);

    int effective_vol = (int)(base_vol * level_to_multiplier(level) + 0.5f);
    if (effective_vol > 100) {
        effective_vol = 100;
    } else if (effective_vol < 0) {
        effective_vol = 0;
    }

    /* aui_player_vol_set() applies the volume directly without touching
     * the persisted VOLUME_SAVE_KV_NAME entry, unlike smtaudio_vol_set() --
     * auto-adjustments must never overwrite the user's saved preference. */
    aui_player_vol_set(SMTAUDIO_TYPE_ALL, effective_vol);
}
