// ADR-0022 持久化接口：cfg 用户配置、白名单与 cfg/系统 NVS 双副本。
// 仅启动或任务上下文，NVS 调用可阻塞，不可在 ISR 中调用。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "nvs.h"

#define NVS_STORE_SN_MAX            (16U) /* 不含结尾 0 */
#define NVS_STORE_ALARM_MAX         (4U)
#define NVS_STORE_ADDR_LEN          (6U)
#define NVS_STORE_SYSTEM_PARTITION  "nvs"
#define NVS_STORE_CFG_PARTITION     "spiffs"
#define NVS_STORE_BACKUP_NAMESPACE  "facbk"
#define NVS_STORE_NET_NAMESPACE     "net"
#define NVS_STORE_NTRIP_NAMESPACE   "ntrip"
#define NVS_STORE_FENCE_NAMESPACE   "fence"
#define NVS_STORE_MIG_NAMESPACE     "mig"
#define NVS_STORE_PN_BYTES          (16U)
#define NVS_STORE_FACTORY_NAMESPACE "fac"
#define NVS_STORE_KEY_REG           "reg"
#define NVS_STORE_REG_MAX           (64U) /* 注册码预留槽位容量（字节） */

#define NVS_STORE_EEPROM_MAX (2560U)

typedef esp_err_t (*nvs_store_recover_fn)(uint8_t *buffer, size_t size);
typedef esp_err_t (*nvs_store_boot_scratch_fn)(nvs_store_recover_fn recover);

/**
 * @brief 登记本次启动异常恢复的临时缓冲提供者，由下一次 init 消费。
 * @param with_scratch: 立即调用 recover 并在其返回后回收缓冲的函数；分配失败时仍以 NULL、0 调用。
 * @note 仅启动任务调用，可选，非 ISR。
 */
void nvs_store_set_boot_scratch(nvs_store_boot_scratch_fn with_scratch);

/**
 * @brief 初始化系统与 cfg 实例，处理特定初始化错误并修复身份双副本。
 * @retval ESP_OK cfg 初始化成功；系统实例失败单独记 E。
 * @retval 其他 cfg 初始化或擦除错误。
 * @note 每次启动仅由启动任务执行一次；可阻塞，非 ISR。
 */
esp_err_t nvs_store_init(void);

/**
 * @brief 按 cfg 分区标签打开一个命名空间。
 * @param name: NVS 命名空间。
 * @param mode: 只读或读写模式。
 * @param[out] handle: 成功时由调用方关闭的句柄。
 * @retval NVS 打开结果。
 * @note 任意任务，可阻塞，非 ISR。
 */
esp_err_t nvs_store_open_cfg(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle);

/**
 * @brief 优先读取 cfg SN，缺失或不可读时尝试系统副本。
 * @param[out] out: 以 NUL 结尾的字符缓冲，失败时清空。
 * @param out_size: 缓冲容量，至少 NVS_STORE_SN_MAX + 1 字节。
 * @retval ESP_OK 已读取；其他为参数或 NVS 错误。
 * @note 任务上下文，可阻塞，非 ISR。
 */
esp_err_t nvs_store_get_sn(char *out, size_t out_size);

/**
 * @brief 写入并提交两份 SN，cfg 为权威副本。
 * @param sn: 长度 1..NVS_STORE_SN_MAX 的 NUL 结尾字符串。
 * @retval ESP_OK 两份均已提交；其他为参数或 NVS 错误。
 * @note 由存储任务串行调用，可阻塞，非 ISR。
 */
esp_err_t nvs_store_set_sn(const char *sn);

/**
 * @brief 优先读取 cfg PN，缺失或不可读时尝试系统副本。
 * @param[out] out: 16 字节定长 PN 缓冲。
 * @retval ESP_OK 已读取；其他为参数或 NVS 错误。
 * @note 任务上下文，可阻塞，非 ISR。
 */
esp_err_t nvs_store_get_pn(uint8_t out[NVS_STORE_PN_BYTES]);

/**
 * @brief 写入并提交两份 PN，保留定长字段的原始字节。
 * @param pn: 16 字节定长 PN。
 * @retval ESP_OK 两份均已提交；其他为参数或 NVS 错误。
 * @note 由存储任务串行调用，可阻塞，非 ISR。
 */
esp_err_t nvs_store_set_pn(const uint8_t pn[NVS_STORE_PN_BYTES]);

/**
 * @brief  读取报警器白名单，每项为 6 字节地址
 * @param[out] addrs: 输出行数组，最多 max 项；仅 max==0 时可传 NULL
 * @param  max: addrs 可容纳的条目数
 * @param[out] count: 实际读出的条目数（≤ max）；参数合法但读取失败或空白名单时为 0，INVALID_ARG 时不写入
 * @retval ESP_OK 读取成功，或白名单不存在/为空（count=0）
 * @retval ESP_ERR_INVALID_ARG count 为 NULL，或 addrs 为 NULL 且 max≠0
 * @retval ESP_ERR_INVALID_SIZE blob 长度不是 6 的倍数，或超过 NVS_STORE_ALARM_MAX×6
 * @retval 其他 nvs_open/nvs_get_blob 错误码
 * @note   非 ISR 安全；nvs 调用会阻塞；使用固定 24 字节栈缓冲，不分配堆
 */
esp_err_t nvs_store_get_alarms(uint8_t (*addrs)[NVS_STORE_ADDR_LEN], size_t max, size_t *count);

/**
 * @brief  覆盖写报警器白名单；count=0 时删除该键
 * @param  addrs: count 行 6 字节地址；count=0 时可传 NULL
 * @param  count: 条目数，不得超过 NVS_STORE_ALARM_MAX
 * @retval ESP_OK 已写入或删除并 commit
 * @retval ESP_ERR_INVALID_ARG count 超限，或 addrs 为 NULL 且 count≠0
 * @retval 其他 nvs_open/nvs_set_blob/nvs_erase_key/nvs_commit 错误码；未 commit 时旧白名单不变
 * @note   非 ISR 安全；nvs 调用与 commit 会阻塞；不分配堆
 */
esp_err_t nvs_store_set_alarms(const uint8_t (*addrs)[NVS_STORE_ADDR_LEN], size_t count);

/**
 * @brief 清空 cfg 用户命名空间和系统 NVS 的蓝牙、Wi-Fi 配置。
 * @retval ESP_OK 擦除完成；其他为底层错误码。
 * @note 仅在持久化写入已停止的任务上下文调用，可阻塞，非 ISR。成功后重启。
 */
esp_err_t nvs_store_erase_user(void);

/**
 * @brief 清空 cfg 的 fac 与系统 NVS 的 facbk 命名空间。
 * @retval ESP_OK 擦除完成；其他为底层错误码。
 * @note 仅在持久化写入已停止的任务上下文调用，可阻塞，非 ISR。成功后重启。
 */
esp_err_t nvs_store_erase_factory(void);

#ifdef __cplusplus
}
#endif
