/*
 * Copyright (C) 2019-2022 Alibaba Group Holding Limited
 */

#include <aos/cli.h>

extern void cli_reg_cmd_kvtool(void);
extern void cli_reg_cmd_ping(void);
extern void cli_reg_cmd_iperf(void);
extern void cli_reg_cmd_ifconfig(void);
extern void cli_reg_cmd_record(void);
extern void cli_reg_cmd_pcminput(void);
extern void cli_reg_cmd_appsys(void);
extern void cli_reg_cmd_voice(void);
extern void cli_reg_cmd_keymsg(void);
extern void cli_reg_cmd_display(void);
extern void cli_reg_cmd_status_event(void);
extern void cli_reg_cmd_player(void);
extern void cli_reg_cmd_eqset(void);
extern void cli_reg_cmd_gpio(void);
extern void cli_reg_cmd_pwm(void);
extern void cli_reg_cmd_gadc(void);
extern void cli_reg_cmd_logipc(void);
extern void cli_reg_cmd_adb_config(void);
extern void cli_reg_cmd_fstst(void);
extern void cli_reg_cmd_nvram(void);
extern void cli_reg_cmd_factory(void);
extern void cli_reg_cmd_clock(void);
extern void cli_reg_cmd_kwstest(void);
extern int ble_cli_register(void);
extern void cli_reg_cmd_ps(void);
extern void cli_reg_cmd_free(void);
extern void cli_reg_cmd_codectest(void);
extern void cli_reg_cmd_wyoming(void);
extern void cli_reg_cmd_esphome_va_test(void);
extern void cli_reg_cmd_esphome_wake_enable(void);
extern void cli_reg_cmd_improv(void);
extern void cli_reg_cmd_mdns(void);

void app_cli_init(void)
{
    cli_reg_cmd_ps();
    cli_reg_cmd_free();
    cli_reg_cmd_kvtool();
    cli_reg_cmd_ping();
    //cli_reg_cmd_iperf();
    cli_reg_cmd_ifconfig();
    cli_reg_cmd_record();
    cli_reg_cmd_pcminput();
    cli_reg_cmd_appsys();
    cli_reg_cmd_voice();
    cli_reg_cmd_rgbmsg();
    cli_reg_cmd_keymsg();
    cli_reg_cmd_status_event();
    cli_reg_cmd_player();

#ifdef CONFIG_BOARD_AUDIO
    cli_reg_cmd_eqset();
#endif

    cli_reg_cmd_gpio();
    cli_reg_cmd_pwm();

#ifndef CONFIG_HAL_ADC_DISABLED
    cli_reg_cmd_gadc();
#endif

#ifdef CONFIG_COMP_IPC
    cli_reg_cmd_logipc();
#endif

    cli_reg_cmd_adb_config();
    cli_reg_cmd_fstst();

#ifdef CONFIG_STANDALONE_NVRAM
    cli_reg_cmd_nvram();
#endif
    cli_reg_cmd_factory();

    //ble_cli_register();

    cli_reg_cmd_clock();

#ifdef CONFIG_KWS_TEST
    cli_reg_cmd_kwstest();
#endif
    //cli_reg_cmd_codectest();
    network_netutils_iperf_cli_register();

    cli_reg_cmd_wyoming();
    cli_reg_cmd_improv();
    cli_reg_cmd_mdns();
    cli_reg_cmd_esphome_va_test();
    cli_reg_cmd_esphome_wake_enable();
}
