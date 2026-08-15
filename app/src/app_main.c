/*
 * Copyright (C) 2019-2022 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <aos/kernel.h>
#include <aos/kv.h>
#include <smart_audio.h>
#include <ulog/ulog.h>
#include <aos/cli.h>
#include <aos/yloop.h>
#include <sys_clk.h>
#include <board.h>

#include "sys/app_sys.h"
#include "player/app_player.h"
#include "wifi/app_net.h"
#include "key_msg/app_key_msg.h"
#include "event_mgr/app_event.h"
#include "wyoming/wyoming.h"
#include "mbedtls/threading.h"
#include "app_main.h"
#include "version.h"
#include "bl606p_clock.h"
#include <bl_efuse.h>
#include "display/pwm_led/pwm_led.h"
//#include "aws_iot_mqtt/app_service.h"
#define TAG "main"

extern void thirdparty_app_init(void);
extern int volume2db2regval(int val);
extern void cxx_system_init(void);

int smartspeaker_main(int argc, char *argv[])
{

#if defined(CONDIF_APP_AMAZON_ENABLE) && (CONDIF_APP_AMAZON_ENABLE == 1)
    volume2db2regval(60);
#endif

    cxx_system_init();

    mbedtls_threading_set_alt(threading_mutex_init,
            threading_mutex_free,
            threading_mutex_lock,
            threading_mutex_unlock);

    board_yoc_init();

    app_sys_init();

    /* KV is only initialized as part of app_sys_init() above -- reload
     * anything that tried to read its persisted state earlier during
     * board_yoc_init() (light_show_state_init() ran too early to see it). */
    led_idle_reload_from_kv();

    // test();
    // app_sys_set_boot_reason_cache(BOOT_REASON_FACTORY_MODE);

#if CONFIG_STDIO_USB
    if (app_sys_get_boot_reason() != BOOT_REASON_FACTORY_MODE) {
        extern int usb_console_cdc_acm_init(uint8_t busid, uintptr_t reg_base);
        usb_console_cdc_acm_init(0, 0);
    }
#endif

    app_cli_init();

    printf("git commit:"BL606P_GIT_VERSION"\r\n");
    printf("fw  commit:"BL606P_WIFI_FW_VERSION"\r\n");
    printf("phy commit:"BL606P_PHY_VERSION"\r\n");
    printf("DEFAULT_SOFTWARE_VER: %s \r\n", DEFAULT_SOFTWARE_VER);

#if CONFIG_BT_RAMBTDUMP_DEBUG
    printf("erase flash, please wait sometimes!\r\n");
#endif
    LOGI("main", "version: %s build time: %s, %s clock=%uHz psram_clk:%uHZ\r\n", DEFAULT_SOFTWARE_VER, __DATE__, __TIME__, soc_get_cur_cpu_freq(),
            Clock_Peripheral_Clock_Get(BL_PERIPHERAL_CLOCK_PSRAMB));
    //aos_msleep(5000);

#if CONFIG_BT_RAMBTDUMP_DEBUG
    bl_flash_erase(0x5BB000, 0x57F000);
#endif
    app_event_init();

    app_key_msg_init();

    app_speaker_init();

    app_player_init();

    light_show_state_msg_send(LIGHT_SHOW_STARTUP, NULL);
    aos_msleep(1000);

#if defined(CONFIG_WIFI_NET) && (CONFIG_WIFI_NET == 1)
    app_network_init();
#endif

    // app_mic_init();

    board_app_init();


    if (app_sys_get_boot_reason() != BOOT_REASON_FACTORY_MODE) {
        wyoming_init();
        wyoming_start();
    } else {
        printf("!FCT!READY\r\n");
    }
    /* Re-apply the persisted volume to the codec's hardware gain register --
     * this is the register that actually drives audible output (see
     * app_key_msg.c / mqtt_client.c), independent of smtaudio's own restore
     * of its aui_player-side volume. Previously hardcoded to 70 here,
     * silently overriding whatever the user had last set. */
    int boot_vol = SMART_AUDIO_DEFAULT_VOLUME;
    aos_kv_getint(VOLUME_SAVE_KV_NAME, &boot_vol);
    volume2db2regval(boot_vol);

    return 0;
}
