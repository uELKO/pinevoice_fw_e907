#include <aos/kv.h>
#include <smart_audio.h>

#include "auto_volume.h"

/* Bounded so ambient-driven adjustments can never drown out or fall below
 * a sane range of the user's actual saved volume. */
#define AUTO_VOL_MIN_MULT   0.75f
#define AUTO_VOL_MAX_MULT   1.25f

/* Ambient envelope calibration (raw int16 RMS-ish scale, see agc.c on the
 * C906 side) -- tune against real hardware/room levels. */
#define AMBIENT_QUIET_LEVEL   200.0f
#define AMBIENT_LOUD_LEVEL   3000.0f

static float level_to_multiplier(float level)
{
    if (level <= AMBIENT_QUIET_LEVEL) {
        return AUTO_VOL_MIN_MULT;
    }
    if (level >= AMBIENT_LOUD_LEVEL) {
        return AUTO_VOL_MAX_MULT;
    }

    float t = (level - AMBIENT_QUIET_LEVEL) / (AMBIENT_LOUD_LEVEL - AMBIENT_QUIET_LEVEL);
    return AUTO_VOL_MIN_MULT + t * (AUTO_VOL_MAX_MULT - AUTO_VOL_MIN_MULT);
}

void auto_volume_on_ambient_level(float level)
{
    /* Don't nudge the volume mid-playback; wait for the next report once
     * idle again so the change isn't audible as a jump. */
    if (smtaudio_get_state() == SMTAUDIO_STATE_PLAYING) {
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
