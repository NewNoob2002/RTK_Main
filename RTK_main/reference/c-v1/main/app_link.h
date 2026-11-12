/**
 * @file    app_link.h
 * @brief   链路任务：SPP 事件 → 角色管理 → 命令分发，以及控制台命令队列。
 *
 * link_mgr/cmd_ctx/alarm_src 只在链路任务内访问；控制台通过 app_link_post_cmd
 * 把请求投递给链路任务（单一所有者）。
 *
 * Generated-by: omp + DeepSeek-V4.1-Flash
 * Owner: UNKNOWN（待人工审查签字）
 */

#ifndef APP_LINK_H
#define APP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "alarm_msg.h"
#include "cmd_disp.h"
#include "esp_err.h"
#include "link_mgr.h"
#include "store_nvs.h"

#define APP_CMD_SN_MAX (STORE_SN_MAX + 1U)

typedef enum
{
    APP_CMD_STATUS = 0,
    APP_CMD_ALARM_SET,
    APP_CMD_ALARM_CLEAR,
    APP_CMD_PAIR,
    APP_CMD_WL_CLEAR,
    APP_CMD_SN
} app_cmd_id_t;

typedef struct
{
    app_cmd_id_t id;
    uint8_t      state;
    uint8_t      level;
    char         sn[APP_CMD_SN_MAX];
    TaskHandle_t reply_task; /* 非 NULL 时链路任务执行完后通知调用方 */
} app_cmd_t;

/* 组合根在创建链路任务前初始化，链路任务运行期间只由它访问。 */
extern link_mgr_t  g_app_link;
extern cmd_ctx_t   g_app_ctx;
extern alarm_src_t g_app_alarm;

link_policy_t app_link_policy (void);
char const *  app_link_policy_name (void);

/** 创建静态命令队列与链路任务；bt_spp_init 成功后调用。 */
esp_err_t app_link_start (void);

bool app_link_is_running (void);

/** 投递控制台命令；队列满或未启动返回 false。 */
bool app_link_post_cmd (app_cmd_t const * p_cmd, uint32_t timeout_ms);

#endif /* APP_LINK_H */

/*** end of file ***/
