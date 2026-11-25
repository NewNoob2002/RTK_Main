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

#define SPP_SERVER_PAIR_NAME_BYTES (17U)

/** 携带请求序号的只读确认快照，id 为零表示无待确认请求。 */
typedef struct {
    uint32_t id;
    uint32_t remaining_ms;
    uint32_t number;
    uint8_t bda[SPP_SERVER_ADDR_LEN];
    char name[SPP_SERVER_PAIR_NAME_BYTES];
    bool has_number;
} spp_server_pair_snapshot_t;

/** @brief 读取尚未决定的请求。 @param out: 快照输出。 @note 任务上下文，短临界区。 */
void spp_server_pair_snapshot(spp_server_pair_snapshot_t *out);

/**
 * @brief 处理指定的待确认请求，拒绝已换代的请求序号。
 * @param id: 快照请求序号，必须非零。
 * @param accept: 接受或拒绝。
 * @retval 是否成功提交。
 */
bool spp_server_pair_decide_for(uint32_t id, bool accept);

/**
 * @brief 登记或轮询 GAP 扫描模式请求，由 worker 执行 SDK 投递。
 * @param paused: true 关闭可发现和可连接，false 恢复。
 * @retval ESP_ERR_NOT_FINISHED 已登记或等待 worker。
 * @retval ESP_OK SDK 已接收请求，射频状态需由实板验证。
 * @retval 其他 驱动未就绪或 SDK 拒绝投递。
 * @note 仅 kLink 调用，非阻塞，非 ISR。
 */
esp_err_t spp_server_scan_pause(bool paused);

#ifdef __cplusplus
}
#endif
