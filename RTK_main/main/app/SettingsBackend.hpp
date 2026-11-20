// 设置服务的应用适配层。绑定在启动期，投递适配器可跨任务调用，非 ISR。
#pragma once

#include "settings/Service.hpp"

namespace app {

struct AppContext;

/** @brief 启动期绑定 DataCenter 与执行器。 @param ctx: 应用上下文。 @note 启动任务独占，非 ISR。 */
void bindSettings(AppContext &ctx) noexcept;

/** @brief 投递归属模块命令。 @param ctx: 后端上下文。 @param owner: 归属。 @param cmd: 命令。 @param caller: 调用方。 @param[out] out: 凭据。 @retval 投递结果。 @note 任意任务，非 ISR。 */
[[nodiscard]] fw::Result postSettingCommand(void *ctx, settings::Owner owner,
                                            const fw::Command &cmd, settings::Caller caller,
                                            settings::Ticket &out) noexcept;

} // namespace app
