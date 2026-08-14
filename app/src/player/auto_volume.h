#ifndef AUTO_VOLUME_H_
#define AUTO_VOLUME_H_

/* Loads the persisted min/max range (falls back to defaults if unset). */
void auto_volume_init(void);

/* Called with a new smoothed ambient mic level (raw envelope scale, see
 * agc_state_t on the C906 side) roughly once per second. Nudges playback
 * volume within a bounded range of the user's saved base volume. */
void auto_volume_on_ambient_level(float level);

/* Bounds as a percentage of the user's saved base volume, e.g. 75/125 means
 * the auto-adjusted volume never leaves [75%, 125%] of the base volume.
 * Persisted to KV so changes (e.g. via the MQTT card) survive reboots. */
void auto_volume_set_range(int min_pct, int max_pct);
void auto_volume_get_range(int *min_pct, int *max_pct);

/* Call whenever the volume changes through a manual path (buttons, MQTT).
 * Suppresses auto_volume_on_ambient_level() for a short cooldown afterwards
 * -- otherwise an ambient report landing between two button presses nudges
 * the live volume out from under the user, and the next press computes its
 * +/-10 off that nudged value instead of what they actually last set. */
void auto_volume_notify_manual_change(void);

#endif
