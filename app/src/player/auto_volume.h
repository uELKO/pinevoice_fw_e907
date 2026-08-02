#ifndef AUTO_VOLUME_H_
#define AUTO_VOLUME_H_

/* Called with a new smoothed ambient mic level (raw envelope scale, see
 * agc_state_t on the C906 side) roughly once per second. Nudges playback
 * volume within a bounded range of the user's saved base volume. */
void auto_volume_on_ambient_level(float level);

#endif
