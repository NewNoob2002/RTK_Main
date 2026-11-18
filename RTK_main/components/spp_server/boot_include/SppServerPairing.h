/* 配对策略注入与手动确认，产品组合根使用的非冻结 C 接口。 */
#pragma once

#include "SppServer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPP_SERVER_PAIR_DENY,
    SPP_SERVER_PAIR_TRUSTED,
    SPP_SERVER_PAIR_ALLOW,
    SPP_SERVER_PAIR_CONFIRM,
} spp_server_pair_policy_t;

typedef spp_server_pair_policy_t (*spp_server_pair_query_t)(const uint8_t *addr);

/**
 * @brief 注入配对查询函数。
 * @param query: 查询函数，在 SPP worker 调用，可以短时加锁，不阻塞。
 * @note 启动时、蓝牙初始化之前绑定。
 */
void spp_server_set_pair_query(spp_server_pair_query_t query);

/**
 * @brief 手动处理挂起的配对请求。
 * @param accept: true 接受，false 拒绝。
 * @retval true 已受理；false 当前无待确认请求。
 * @note 控制台或链路任务，非 ISR；通过 worker 投递 GAP 回复。
 */
bool spp_server_pair_decide(bool accept);

#ifdef __cplusplus
}
#endif
