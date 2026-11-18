// 固件入口（ADR-0009）：只做转交，启动步骤、降级与摘要全部在 app/BootFlow.cpp 的步骤表里。
// 线程：ESP-IDF 启动任务调用一次；可阻塞（等待消费者到达启动屏障），非 ISR。
#include "AppContext.hpp"

/**
 * @brief 执行固件组合根的启动流程
 * @note 仅 IDF main 任务调用一次，可阻塞，非 ISR。
 */
extern "C" void app_main(void)
{
    app::appContext().boot();
}
