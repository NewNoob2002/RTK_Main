/**
 * @file    app_cons.c
 * @brief   UART0 esp_console REPL 与测试命令。
 *
 * 控制台只负责解析参数并通过 app_link_post_cmd() 投递；不直接访问
 * link_mgr/cmd_ctx/alarm_src。status/alarm/pair/wl 的输出来自链路任务。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart_vfs.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"

#include "alarm_msg.h"
#include "app_cons.h"
#include "app_link.h"
#include "store_nvs.h"

#define APP_CONS_TAG "cons"
#define APP_CONS_PROMPT "rtk> "
#define APP_CONS_DEC_BASE (10)
#define APP_CONS_WAIT_MS (1000U)
#define APP_CONS_POST_MS (100U)
#define APP_CONS_ARGC_ONE (1)
#define APP_CONS_ARGC_TWO (2)
#define APP_CONS_ARGC_FOUR (4)
#define APP_CONS_ARGV_STATE (2U)
#define APP_CONS_ARGV_LEVEL (3U)

static bool
run_cmd (app_cmd_t * p_cmd)
{
    if (!app_link_is_running())
    {
        printf("link task not running\n");
        return false;
    }

    p_cmd->reply_task = xTaskGetCurrentTaskHandle();
    (void) ulTaskNotifyTake(pdTRUE, 0);
    if (!app_link_post_cmd(p_cmd, APP_CONS_POST_MS))
    {
        printf("link busy\n");
        return false;
    }
    if (pdTRUE != ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_CONS_WAIT_MS)))
    {
        printf("timeout\n");
        return false;
    }
    return true;
}

static int
cmd_status (int argc, char ** argv)
{
    app_cmd_t cmd;

    (void) argc;
    (void) argv;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id = APP_CMD_STATUS;
    (void) run_cmd(&cmd);
    return 0;
}

static int
cmd_alarm (int argc, char ** argv)
{
    app_cmd_t cmd;
    long      state = 0;
    long      level = 0;

    memset(&cmd, 0, sizeof(cmd));

    if ((APP_CONS_ARGC_TWO == argc) && (0 == strcmp(argv[1], "clear")))
    {
        cmd.id = APP_CMD_ALARM_CLEAR;
        (void) run_cmd(&cmd);
        return 0;
    }
    if ((APP_CONS_ARGC_FOUR != argc) || (0 != strcmp(argv[1], "set")))
    {
        printf("usage: alarm set <state 0-2> <level 0-3> | alarm clear\n");
        return 1;
    }

    state = strtol(argv[APP_CONS_ARGV_STATE], NULL, APP_CONS_DEC_BASE);
    level = strtol(argv[APP_CONS_ARGV_LEVEL], NULL, APP_CONS_DEC_BASE);
    /* 常量 ≤ 3，转 long 仅扩宽 */
    if ((state < 0) || (state > (long) ALARM_STATE_NO_FIX) || (level < 0) ||
        (level > (long) ALARM_LEVEL_MAX))
    {
        printf("invalid state/level\n");
        return 1;
    }

    cmd.id = APP_CMD_ALARM_SET;
    /* 已校验 0..2 / 0..3，u8 截断安全 */
    cmd.state = (uint8_t) state;
    cmd.level = (uint8_t) level;
    (void) run_cmd(&cmd);
    return 0;
}

static int
cmd_pair (int argc, char ** argv)
{
    app_cmd_t cmd;

    if (APP_CONS_ARGC_ONE != argc)
    {
        printf("usage: pair\n");
        return 1;
    }

    (void) argv;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id = APP_CMD_PAIR;
    (void) run_cmd(&cmd);
    return 0;
}

static int
cmd_wl (int argc, char ** argv)
{
    app_cmd_t cmd;

    if ((APP_CONS_ARGC_TWO != argc) || (0 != strcmp(argv[1], "clear")))
    {
        printf("usage: wl clear\n");
        return 1;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.id = APP_CMD_WL_CLEAR;
    (void) run_cmd(&cmd);
    return 0;
}

static int
cmd_sn (int argc, char ** argv)
{
    app_cmd_t cmd;

    if (APP_CONS_ARGC_TWO != argc)
    {
        printf("usage: sn <string>\n");
        return 1;
    }
    if (strlen(argv[1]) > STORE_SN_MAX)
    {
        printf("sn too long (max %u)\n", (unsigned) STORE_SN_MAX);
        return 1;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.id = APP_CMD_SN;
    (void) snprintf(cmd.sn, sizeof(cmd.sn), "%s", argv[1]);
    (void) run_cmd(&cmd);
    return 0;
}

static int
cmd_policy (int argc, char ** argv)
{
    (void) argc;
    (void) argv;
    printf("link policy: %s\n", app_link_policy_name());
    return 0;
}

static esp_console_cmd_t const g_cons_cmds[] = {
    {.command = "status",
     .help    = "print sessions, whitelist, alarm state, heap and stack",
     .func    = cmd_status},
    {.command = "alarm",
     .help    = "alarm set <state 0-2> <level 0-3> | alarm clear",
     .func    = cmd_alarm},
    {.command = "pair", .help = "open 60 s pairing window", .func = cmd_pair},
    {.command = "wl", .help = "wl clear", .func = cmd_wl},
    {.command = "sn",
     .help    = "sn <string>: save SN (reboot to apply)",
     .func    = cmd_sn},
    {.command = "policy",
     .help    = "print current link policy",
     .func    = cmd_policy},
};

esp_err_t
app_cons_start (void)
{
    esp_console_repl_t *          p_repl   = NULL;
    esp_console_repl_config_t     repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_cfg =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = ESP_OK;

    repl_cfg.prompt = APP_CONS_PROMPT;
    err             = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &p_repl);
    if (ESP_OK != err)
    {
        ESP_LOGE(APP_CONS_TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t idx = 0U; idx < (sizeof(g_cons_cmds) / sizeof(g_cons_cmds[0]));
         idx++)
    {
        esp_err_t reg_err = esp_console_cmd_register(&g_cons_cmds[idx]);

        if (ESP_OK != reg_err)
        {
            ESP_LOGE(APP_CONS_TAG, "register %s failed: %s",
                     g_cons_cmds[idx].command, esp_err_to_name(reg_err));
        }
    }

    err = esp_console_start_repl(p_repl);
    if (ESP_OK != err)
    {
        ESP_LOGE(APP_CONS_TAG, "console start failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

/*** end of file ***/
