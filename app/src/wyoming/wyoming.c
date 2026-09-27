// Copyright 2026 Marek Kraus (@gamelaster / @gamiee)
// SPDX-License-Identifier: Apache-2.0

#include <aos/cli.h>
#include <aos/kernel.h>
#include <ulog/ulog.h>
#include <wyoming/satellite.h>
#include <yoc/mic.h>
#include <avutil/named_straightfifo.h>
#include <player.h>
#include "../display/pwm_led/pwm_led.h"
#include "../player/app_player.h"
#include "../player/auto_volume.h"
#include "../mqtt/http_config.h"
#include "../mqtt/mqtt_client.h"
#include "../esphome_api/esphome_mdns.h"
#include <esphome_api/esphome_api.h>
#include <esphome_api/esphome_proto.h>
#include "wyoming.h"
#include "../version.h"
#include <bl_efuse.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define AUD_SAMP_CNT 5
static uint8_t audio_data[2][320*AUD_SAMP_CNT];
static uint8_t audio_data_sel = 0;
static aos_sem_t audio_sem;
static uint8_t buffers_cnt = 0;
static uint8_t data_ready = 0;

#define TAG "wyoming"

// Safety switch for the real wake-word path (see cmd_esphome_wake_enable
// below). Defaults to false (Wyoming, unchanged behavior) and is NEVER
// persisted to KV -- it always resets to Wyoming-only on reboot, so a bad
// live test of the ESPHome path can't strand daily use in a broken mode; a
// power cycle alone recovers, no reflash needed.
static volatile bool s_esphome_wake_enabled = false;
static void esphome_wake_trigger(void);
static void esphome_wake_worker_start(void);

static void mic_evt_cb(int source, mic_event_id_t evt_id, void *data, int size) {
  static uint32_t i = 0;
  switch (evt_id) {
    case MIC_EVENT_SESSION_START: {
      LOGD(TAG, "WAKE UP!!!");
      local_wakeup_audio_play("chime.opus");
      if (s_esphome_wake_enabled) {
        esphome_wake_trigger();
        break;
      }
      int32_t ret = wsat_wake_detection();
      if (!app_network_internet_is_connected() || ret == -WSAT_ERROR_SAT_DISCONNECTED) {
        if (!app_network_internet_is_connected()) {
          local_audio_play("wifi-is-disconnected.opus");
        } else {
          local_audio_play("wsat-is-disconnected.opus");
        }
        light_show_state_msg_send(LIGHT_SHOW_ERROR, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
      }
      break;
    }
    case MIC_EVENT_PCM_DATA: {
      memcpy(audio_data[audio_data_sel] + (320 * buffers_cnt), data, size);
      buffers_cnt++;
      if (buffers_cnt >= AUD_SAMP_CNT) {
        if (data_ready) {
          LOGE(TAG, "Data not processed");
        }
        buffers_cnt = 0;
        audio_data_sel = audio_data_sel ? 0 : 1;
        data_ready = 1;
        aos_sem_signal(&audio_sem);
      }
      break;
    }
  }
}

static struct wsat_wake wake = {
  {
    WSAT_COMPONENT_TYPE_WAKE,
    NULL,
    NULL,
    NULL,
    false,
  },
  "alexa"
};

// Set by esphome_run_voice_assistant_session() (used by both the CLI test
// command and, when s_esphome_wake_enabled, the real wake-word path) for the
// duration of one ESPHome voice-assistant turn. Regardless of this flag, the
// existing wsat_mic_write_data() call below always runs unchanged -- this
// only ever adds an extra, independent forward of the same already-captured
// PCM, so Wyoming's own pipeline is never affected by it.
static volatile bool s_esphome_streaming = false;

static void mic_streamer_fn(void *arg)
{
  while (1) {
    aos_sem_wait(&audio_sem, AOS_WAIT_FOREVER);
    static uint8_t data[320*AUD_SAMP_CNT];
    uint8_t data_sel = audio_data_sel ? 0 : 1;
    memcpy(data, audio_data[data_sel], sizeof(data));
    data_ready = 0;
    wsat_mic_write_data(data, sizeof(data));
    if (s_esphome_streaming) {
      esphome_api_send_voice_assistant_audio(data, sizeof(data), false);
    }
  }
}

static int32_t mic_init()
{
  aos_sem_create(&audio_sem, 0, NULL);
  aos_task_t task_handle;
  aos_task_new_ext(&task_handle, "mic_streamer", mic_streamer_fn, (void *)NULL, 4096, AOS_DEFAULT_APP_PRI);

  LOGI(TAG, "MIC_CTRL_START_PCM");
  aui_mic_control(MIC_CTRL_START_PCM);
  return 0;
}

static int32_t mic_destroy()
{
  LOGI(TAG, "MIC_CTRL_STOP_PCM");
  aui_mic_control(MIC_CTRL_STOP_PCM);
  return 0;
}

struct wsat_microphone mic = {
  {
    WSAT_COMPONENT_TYPE_MICROPHONE,
    mic_init,
    mic_destroy,
    NULL,
    false,
  },
  16000,
  2,
  1,
};

static nsfifo_t* g_playback_fifo;
static player_t* g_player;

static void _player_event(player_t *player, uint8_t type, const void *data, uint32_t len)
{
  UNUSED(len);
  UNUSED(data);

  switch (type) {
  case PLAYER_EVENT_ERROR:
    LOGE(TAG, "Error player!");
    player_stop(g_player);
    break;
  case PLAYER_EVENT_START:
    break;
  case PLAYER_EVENT_FINISH:
    LOGD(TAG, "Finish playing! :)");
    player_stop(g_player);
    light_show_state_msg_send(LIGHT_SHOW_READY, NULL);
    nsfifo_close(g_playback_fifo);
    g_playback_fifo = NULL;
    break;
  default:
    break;
  }
}

static int32_t snd_start_stream(uint32_t rate, uint8_t width, uint8_t channels)
{
  // TODO: Check if FIFO is opened or not.

  char fifo_tts_url[128];
  snprintf(fifo_tts_url, sizeof(fifo_tts_url), "fifo://wyo_tts?avformat=rawaudio&avcodec=pcm_s16le&channel=1&rate=%d", rate);

  g_playback_fifo = nsfifo_open(fifo_tts_url, O_CREAT, 1*1024*1024);
  if (NULL == g_playback_fifo) {
    LOGE(TAG, "nsfifo_open fail");
    return;
  }

  player_play(g_player, fifo_tts_url, 0);
  LOGD(TAG, "Start stream %d", rate);
  return 0;
}

static int32_t snd_stop_stream()
{
  nsfifo_set_eof(g_playback_fifo, 0, 1); // set weof
  LOGD(TAG, "Stop stream");
  return 0;
}

static int32_t snd_on_data(uint8_t* data, uint32_t size)
{
  int wlen;
  char *pos;
  uint8_t reof = 0;

  int off = 0;
  int tmp_len;

  while (1) {
    wlen = nsfifo_get_wpos(g_playback_fifo, &pos, 10*1000);
    nsfifo_get_eof(g_playback_fifo, &reof, NULL);
    if (reof) {
      LOGE(TAG, "get wpos err. wlen = %d, reof = %d", wlen, reof);
      return 0;
    }

    if (wlen <= (size - off)) {
      tmp_len = wlen;
    } else {
      tmp_len = size - off;
    }

    if (tmp_len > 0) {
      memcpy(pos, data + off, tmp_len);
      nsfifo_set_wpos(g_playback_fifo, tmp_len);
      off += tmp_len;
    }

    if (wlen == 0) {
      aos_msleep(100);
    }

    if (off == size) {
      break;
    }
  }
  // WiFi stack and other threads did not liked when burst of audio data arrived. This fixes it, but it's not final solution
  aos_task_yield();
  return WSAT_OK;
}

int32_t snd_init()
{
  static int is_player_init = 0;
  if(!is_player_init)
  {
    ply_conf_t ply_cnf;

    player_conf_init(&ply_cnf);
    ply_cnf.resample_rate = 48000;
    ply_cnf.event_cb      = _player_event;
    g_player = player_new(&ply_cnf);

    is_player_init = 1;
    LOGD(TAG, "Player init!");
  }
  return WSAT_OK;
}

int32_t snd_handle_sys_event(enum wsat_sys_event_type type, void* data)
{
  switch (type) {
  case WSAT_SYS_EVENT_SND_AUDIO_START: {
    struct wsat_sys_event_audio_start_params* info = data;
    snd_start_stream(info->rate, info->width, info->channels);
    break;
  }
  case WSAT_SYS_EVENT_SND_AUDIO_DATA: {
    struct wsat_sys_event_buffer_params* buffer = data;
    snd_on_data(buffer->data, buffer->size);
    break;
  }
  case WSAT_SYS_EVENT_SND_AUDIO_END: {
    snd_stop_stream();
  }
  default: break;
  }
  return WSAT_OK;
}

static struct wsat_sound snd = {
  {
    WSAT_COMPONENT_TYPE_SOUND,
    snd_init,
    NULL,
    snd_handle_sys_event,
    false,
  }
};

static int32_t fback_handle_sys_event(enum wsat_sys_event_type type, void* data)
{
  switch (type) {
    case WSAT_SYS_EVENT_SAT_CONNECT:
      light_show_state_set(LIGHT_SHOW_READY);
      break;
    case WSAT_SYS_EVENT_SAT_DISCONNECT:
      light_show_state_set(LIGHT_SHOW_SAT_CONN_PENDING);
      break;
    case WSAT_SYS_EVENT_WAKE_DETECTION:
      light_show_state_msg_send(LIGHT_SHOW_LISTENING, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
      break;
    case WSAT_SYS_EVENT_VOICE_STOP:
      light_show_state_msg_send(LIGHT_SHOW_PROCESSING, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
      break;
    case WSAT_SYS_EVENT_SND_AUDIO_START:
      light_show_state_msg_send(LIGHT_SHOW_ANSWER, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
      break;
    // we handle SND_AUDIO_STOP in actual audio sound stop event in snd impl.
    case WSAT_SYS_EVENT_ERROR:
      light_show_state_set(LIGHT_SHOW_READY);
      light_show_state_msg_send(LIGHT_SHOW_ERROR, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
      break;

  }
  return WSAT_OK;
}

static struct wsat_feedback fback = {
  {
    WSAT_COMPONENT_TYPE_FEEDBACK,
    NULL,
    NULL,
    fback_handle_sys_event,
    true,
  }
};

static void wyoming_server(void *arg)
{
  wsat_init();
  wsat_settings_set(WSAT_SETTING_TYPE_SATELLITE_NAME, "PineVoice");
  wsat_settings_set(WSAT_SETTING_TYPE_SATELLITE_VERSION, DEFAULT_SOFTWARE_VER);
  wsat_mic_set(&mic);
  wsat_snd_set(&snd);
  wsat_wake_set(&wake);
  wsat_fback_set(&fback);
  wsat_run();
}

void wyoming_start(void)
{
  aos_task_t task_handle;
  aos_task_new_ext(&task_handle, "wyoming_server", wyoming_server, (void *)NULL, 4096, AOS_DEFAULT_APP_PRI);
}

// Defined further down, alongside the rest of the esphome_va_test CLI
// command it supports; forward-declared so wyoming_init() (right below) can
// register it.
static void va_test_event_cb(uint32_t event_type, const char *name, const char *value);

void wyoming_init()
{
  aui_mic_register();

  utask_t *task_mic = utask_new("task_mic", 10 * 1024, 20, AOS_DEFAULT_APP_PRI);
  int ret           = aui_mic_init(task_mic);
  aos_msleep(100); // wait for mic to init....
  aui_mic_event_register(mic_evt_cb);
  aui_mic_start();
  wyoming_mdns_advertise_start();
  http_config_start();
  auto_volume_init();
  mqtt_client_start();
  LOGI(TAG, "Wyoming init\r\n");

  // Phase 1 of the ESPHome-native-API migration (see TODO.md). Re-enabled
  // 2026-09-26 after fixing two confirmed bugs found while first testing
  // this: MDNS_MAX_SERVICES was 1 (Wyoming's service filled the only slot,
  // so esphome_mdns_advertise_start() could never register -- bumped to 2
  // in lwipopts.h), and esphome_server_task's accept()-failure retry loop
  // had no backoff (added aos_msleep(200)). Neither was confirmed as the
  // cause of the hang seen on first test (full unresponsiveness, LED stuck
  // on the red error show, reproduced across a hard power cycle) -- this
  // re-enable is to observe the boot live over the console and catch the
  // actual failure point, not a claim that it's fixed.
  {
    static char s_esp_hostname[24];
    static char s_esp_mac[18];
    static esphome_api_device_info_t s_esp_dev_info;
    uint8_t mac[6];

    bl_efuse_read_mac_smart(1, mac, 0);
    snprintf(s_esp_hostname, sizeof(s_esp_hostname), "pinevoice-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_esp_mac, sizeof(s_esp_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esphome_mdns_advertise_start(s_esp_hostname);

    s_esp_dev_info.name = s_esp_hostname;
    s_esp_dev_info.friendly_name = s_esp_hostname;
    s_esp_dev_info.mac_address = s_esp_mac;
    s_esp_dev_info.model = "PineVoice";
    s_esp_dev_info.manufacturer = "Pine64";
    s_esp_dev_info.version = DEFAULT_SOFTWARE_VER;
    esphome_api_set_device_info(&s_esp_dev_info);
    esphome_api_set_voice_assistant_event_callback(va_test_event_cb);
    esphome_api_start();
    esphome_wake_worker_start();
  }
}

void cmd_wyoming(char *wbuf, int wbuf_len, int argc, char **argv) {
  LOGI(TAG, "Starting Wyoming...\r\n");
  wyoming_start();
}

void cli_reg_cmd_wyoming(void) {

  static const struct cli_command cmd_info = {"wyoming", "start Wyoming", cmd_wyoming};
  aos_cli_register_command(&cmd_info);
}

// Separate player instance from Wyoming's own g_player (used for its
// nsfifo-streamed TTS) -- this one just plays a complete HTTP(S) URL
// directly, same as e.g. "smta play <url>" on the CLI. Kept independent so
// nothing about this test path can affect Wyoming's existing, working
// playback pipeline.
static player_t *s_va_test_player = NULL;

static void va_test_player_event(player_t *player, uint8_t type, const void *data, uint32_t len)
{
  UNUSED(data);
  UNUSED(len);
  switch (type) {
  case PLAYER_EVENT_ERROR:
    LOGE(TAG, "esphome_va_test: TTS playback error");
    player_stop(s_va_test_player);
    break;
  case PLAYER_EVENT_FINISH:
    LOGI(TAG, "esphome_va_test: TTS playback finished");
    player_stop(s_va_test_player);
    break;
  default:
    break;
  }
}

static void va_test_play_tts(const char *url)
{
  if (!s_va_test_player) {
    ply_conf_t ply_cnf;
    player_conf_init(&ply_cnf);
    ply_cnf.resample_rate = 48000;
    ply_cnf.event_cb = va_test_player_event;
    s_va_test_player = player_new(&ply_cnf);
  }
  LOGI(TAG, "esphome_va_test: playing TTS from %s", url);
  player_play(s_va_test_player, url, 0);
}

// Registered once in wyoming_init() (see below). Runs on the esphome_api
// connection task -- keep it quick, no blocking network I/O here beyond the
// already-async player_play() call.
static void va_test_event_cb(uint32_t event_type, const char *name, const char *value)
{
  if ((event_type == ESPB_VA_EVENT_STT_END || event_type == ESPB_VA_EVENT_ERROR ||
       event_type == ESPB_VA_EVENT_RUN_END) && s_esphome_streaming) {
    s_esphome_streaming = false;
    esphome_api_send_voice_assistant_audio(NULL, 0, true);
    LOGI(TAG, "esphome_va: stopped audio streaming (event_type=%u)", (unsigned)event_type);
  }

  if (event_type == ESPB_VA_EVENT_TTS_END && strcmp(name, "url") == 0) {
    va_test_play_tts(value);
  }
}

// One full ESPHome voice-assistant turn: request -> stream mic audio until
// an event_cb-driven stop -> (TTS playback happens asynchronously via the
// event callback once TTS_END arrives). Shared by the CLI test command and,
// when s_esphome_wake_enabled, the real wake-word path (see
// esphome_wake_worker_fn below) -- exactly what was verified manually
// against real HA on 2026-09-26 (see TODO.md), just reachable from two
// different triggers now.
static void esphome_run_voice_assistant_session(void)
{
  uint32_t port = 0;
  bool va_error = false;
  LOGI(TAG, "esphome_va: sending VoiceAssistantRequest...");
  // USE_WAKE_WORD asks HA to run its OWN wake-word stage server-side, which
  // errors with "wake-engine-missing" on this satellite (on-device wake
  // word only, nothing configured for HA to run) -- confirmed 2026-09-26 by
  // decoding the VoiceAssistantEventResponse ERROR event. USE_VAD alone
  // means "wake word already happened locally, start the pipeline here".
  bool ok = esphome_api_send_voice_assistant_start(
      "", ESPB_VA_REQUEST_USE_VAD, 5000, &port, &va_error);
  if (!ok) {
    LOGE(TAG, "esphome_va: no VoiceAssistantResponse (no HA connection or timeout)");
    local_audio_play("wsat-is-disconnected.opus");
    light_show_state_msg_send(LIGHT_SHOW_ERROR, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
    return;
  }
  LOGI(TAG, "esphome_va: VoiceAssistantResponse port=%u error=%d", (unsigned)port, (int)va_error);
  if (va_error) {
    light_show_state_msg_send(LIGHT_SHOW_ERROR, LIGHT_SHOW_MSG_FLAGS(LIGHT_SHOW_MSG_FLAG_INTERRUPT));
    return;
  }

  LOGI(TAG, "esphome_va: streaming mic audio (stops on STT end)");
  s_esphome_streaming = true;
  // Normally stopped much sooner by va_test_event_cb() on STT_END/ERROR/
  // RUN_END; this is only a safety net against a pipeline that never signals
  // one of those (so we don't stream forever).
  aos_msleep(15000);
  if (s_esphome_streaming) {
    s_esphome_streaming = false;
    esphome_api_send_voice_assistant_audio(NULL, 0, true);
    LOGW(TAG, "esphome_va: no STT-end/error/run-end event within 15s, stopped by timeout");
  }
}

// Manual test path for the ESPHome-native-API voice-assistant flow (see
// TODO.md, "Voice-Assistant-Ablauf"). Lets us verify the flow against real
// HA from the console independent of s_esphome_wake_enabled/the real
// wake-word path.
void cmd_esphome_va_test(char *wbuf, int wbuf_len, int argc, char **argv) {
  esphome_run_voice_assistant_session();
}

void cli_reg_cmd_esphome_va_test(void) {
  static const struct cli_command cmd_info = {"esphome_va_test", "send a test VoiceAssistantRequest", cmd_esphome_va_test};
  aos_cli_register_command(&cmd_info);
}

// Dedicated worker task so mic_evt_cb (called on the mic driver's own task)
// never blocks on the network -- it just signals this semaphore and returns
// immediately, exactly like wsat_wake_detection() itself only posts to
// Wyoming's own async state machine rather than running the turn inline.
static aos_sem_t s_esphome_wake_sem;

static void esphome_wake_worker_fn(void *arg)
{
  while (1) {
    aos_sem_wait(&s_esphome_wake_sem, AOS_WAIT_FOREVER);
    esphome_run_voice_assistant_session();
  }
}

static void esphome_wake_trigger(void)
{
  aos_sem_signal(&s_esphome_wake_sem);
}

// Called once from wyoming_init() -- always spawns the worker task
// regardless of s_esphome_wake_enabled (an idle task blocked on a semaphore
// costs nothing worth gating), so flipping the switch on later needs no
// further setup.
static void esphome_wake_worker_start(void)
{
  aos_sem_new(&s_esphome_wake_sem, 0);
  aos_task_t task_handle;
  aos_task_new_ext(&task_handle, "esphome_wake", esphome_wake_worker_fn, (void *)NULL, 4096, AOS_DEFAULT_APP_PRI);
}

// Toggles which backend the real wake-word event triggers -- see
// s_esphome_wake_enabled above for why this is intentionally not persisted.
void cmd_esphome_wake_enable(char *wbuf, int wbuf_len, int argc, char **argv) {
  if (argc >= 2) {
    s_esphome_wake_enabled = (atoi(argv[1]) != 0);
  }
  LOGI(TAG, "esphome_wake_enable: real wake word now uses %s (resets to Wyoming on reboot)",
       s_esphome_wake_enabled ? "ESPHome" : "Wyoming");
}

void cli_reg_cmd_esphome_wake_enable(void) {
  static const struct cli_command cmd_info = {"esphome_wake_enable", "0|1: real wake word triggers Wyoming(0, default) or ESPHome(1)", cmd_esphome_wake_enable};
  aos_cli_register_command(&cmd_info);
}
