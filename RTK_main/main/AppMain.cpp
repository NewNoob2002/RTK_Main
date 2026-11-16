// 固件入口（R8a）：只做转交，启动步骤、降级与摘要全部在 app/BootFlow.cpp 的步骤表里。
// 线程：ESP-IDF 启动任务调用一次；可阻塞（等待消费者到达启动屏障），非 ISR。
#include "AppContext.hpp"

extern "C" void app_main(void)
{
    app::app_context().boot();
}
