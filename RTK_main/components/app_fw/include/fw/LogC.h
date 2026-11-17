/* 组件分级日志的 C 接口（ADR-0009 §2.12）：供 spp_server、nvs_store 等 C 组件使用，语义与 fw/Log.hpp 的
 * FW_LOG* 完全相同（同一过滤、同一通路、同一行格式）。类别与级别常量的数值与 fw::TraceCat / fw::LogLevel
 * 一致，由 Log.cpp 的 static_assert 保证。线程约束同 fw/Log.hpp。冻结接口（ADR-0009 §3）。 */
#pragma once

#include <stdint.h>

/* 编译期最高日志级别：0=E 1=W 2=I 3=D；ESP-IDF 下由 CONFIG_RTK_LOG_LEVEL 经 CMake 定义。 */
#ifndef FW_LOG_MAX_LEVEL
#define FW_LOG_MAX_LEVEL 3
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 日志类别（与 fw::TraceCat 数值一致）。 */
enum {
    FW_LOG_CAT_LINK = 0,
    FW_LOG_CAT_SPP = 1,
    FW_LOG_CAT_RTCM = 2,
    FW_LOG_CAT_PROTO = 3,
    FW_LOG_CAT_ALARM = 4,
    FW_LOG_CAT_STORE = 5,
    FW_LOG_CAT_EXEC = 6,
    FW_LOG_CAT_GNSS = 7,
    FW_LOG_CAT_SYS = 8,
};

/** 日志级别（与 fw::LogLevel 数值一致）。 */
enum {
    FW_LOG_LEVEL_ERROR = 0,
    FW_LOG_LEVEL_WARN = 1,
    FW_LOG_LEVEL_INFO = 2,
    FW_LOG_LEVEL_DEBUG = 3,
};

/**
 * @brief  记录一行日志（宏 FW_CLOG* 的实现；一般不直接调用）
 * @param  level: FW_LOG_LEVEL_*
 * @param  cat: FW_LOG_CAT_*；越界丢弃
 * @param  fmt: printf 格式
 */
void fw_log_c(int level, int cat, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

/* 宏：高于 FW_LOG_MAX_LEVEL 的调用被常量条件消除（参数不求值）。 */
#define FW_CLOG_AT(level, cat, ...)                                                                \
    do {                                                                                           \
        if ((level) <= FW_LOG_MAX_LEVEL) {                                                         \
            fw_log_c((level), (cat), __VA_ARGS__);                                                 \
        }                                                                                          \
    } while (0)
#define FW_CLOGE(cat, ...) FW_CLOG_AT(FW_LOG_LEVEL_ERROR, cat, __VA_ARGS__)
#define FW_CLOGW(cat, ...) FW_CLOG_AT(FW_LOG_LEVEL_WARN, cat, __VA_ARGS__)
#define FW_CLOGI(cat, ...) FW_CLOG_AT(FW_LOG_LEVEL_INFO, cat, __VA_ARGS__)
#define FW_CLOGD(cat, ...) FW_CLOG_AT(FW_LOG_LEVEL_DEBUG, cat, __VA_ARGS__)
