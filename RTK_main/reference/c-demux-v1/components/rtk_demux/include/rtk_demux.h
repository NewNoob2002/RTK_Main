// 冻结接口（ADR-0004）。实现方不得修改；变更须先修订 ADR-0004。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN
//
// 通用多协议字节流分流器（stream demultiplexer）。
// - 纯 C11，不依赖任何 OS / SDK / 平台头文件，不用 malloc，不用浮点，可在任意 MCU 与 host 编译。
// - 一个实例对应一条字节流（蓝牙会话、GNSS UART、TCP 连接……），由调用方提供状态与缓冲区存储。
// - 协议由“解码器描述符”（rtk_demux_decoder_t）插入；分流器只负责找帧起点、累积字节、
//   失败回放重扫、回调分发与统计，不理解任何具体协议。
// 线程安全：否（一个实例只能由一个任务使用）。ISR 安全：是（无阻塞、无 OS 调用），但回调在 feed 的
// 调用上下文中同步执行，耗时由回调决定。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一个实例最多挂载的解码器数（首字节表用 uint8_t 位图）。 */
#define RTK_DEMUX_MAX_DECODERS (8U)

/** 解码器 step 的结论。 */
typedef enum {
    RTK_DEMUX_STEP_NEED_MORE = 0, /* 还不能下结论；*need 写入下次调用前至少还要追加的字节数（≥1） */
    RTK_DEMUX_STEP_FRAME,         /* frame[0..len) 恰好是一条完整且校验通过的消息 */
    RTK_DEMUX_STEP_REJECT,        /* frame[0..len) 不是本协议的有效前缀（同步/长度/校验失败） */
} rtk_demux_step_t;

/**
 * 解码器描述符。静态常量，可被多个实例共享；解码器自身无可变状态。
 *
 * step 契约（分流器依赖这些性质保证回放重扫正确）：
 * - 纯函数：结论只取决于 frame[0..len) 的内容，不依赖之前的调用；可对同一前缀重复调用。
 * - 只在 len ≥ 1 且 frame[0] 通过 first_byte 时被调用。
 * - 返回 NEED_MORE 时 *need ≥ 1，且 len + *need ≤ max_frame；否则视为实现错误，分流器按 REJECT 处理。
 *   *need 用于批量累积：例如头部收完、长度已知后，一次要求“剩余全部字节”，分流器整块拷贝后再调用一次，
 *   CRC 在帧尾一次性计算。不知道还要多少字节（如 NMEA 等结束符）时写 1。
 * - 返回 FRAME 表示 frame[0..len) 全部属于该消息（不多不少）。
 * - 尽早 REJECT：例如第 2、3 个同步字节不符时立即拒绝，以便同首字节的其他解码器尝试、并减少回放量。
 */
typedef struct {
    const char *name; /* 诊断用，非 NULL */
    uint16_t proto_id; /* 由使用方定义的协议编号，原样透传给 on_frame */
    uint16_t max_frame; /* 本协议允许的最大帧长（字节），≥1；超过的长度字段应在 step 中 REJECT */
    bool (*first_byte)(uint8_t byte); /* 该字节能否作为本协议帧的第一个字节；只在 init 时调用 256 次 */
    rtk_demux_step_t (*step)(const uint8_t *frame, size_t len, size_t *need);
} rtk_demux_decoder_t;

/**
 * 一条完整帧。frame 指向分流器缓冲区，只在回调返回前有效。
 * decoder_index 是该解码器在 init 时 decoders[] 中的下标。
 */
typedef void (*rtk_demux_frame_cb_t)(void *user, const rtk_demux_decoder_t *decoder, uint8_t decoder_index,
                                     const uint8_t *frame, size_t len);

/**
 * 一段不属于任何已挂载协议的字节（按流顺序、连续字节合并后回调，可能分多次）。
 * 用于透传或诊断；不需要时传 NULL，字节仅计数后丢弃。
 */
typedef void (*rtk_demux_unknown_cb_t)(void *user, const uint8_t *bytes, size_t len);

/** 每个解码器的统计。 */
typedef struct {
    uint32_t frames;  /* 成功帧数 */
    uint32_t rejects; /* 该解码器在已认领状态下 REJECT 的次数（含 step 契约违规） */
    uint32_t bytes;   /* 成功帧的总字节数 */
} rtk_demux_decoder_stats_t;

/** 实例统计。 */
typedef struct {
    rtk_demux_decoder_stats_t decoder[RTK_DEMUX_MAX_DECODERS];
    uint32_t unknown_bytes; /* 交给 on_unknown（或丢弃）的字节数 */
    uint32_t resyncs;       /* 所有候选都拒绝后，起点前移重扫的次数 */
    uint32_t overflows;     /* 解码器要求的长度超过缓冲区容量而被强制拒绝的次数 */
    uint32_t aborts;        /* rtk_demux_abort 丢弃半帧的次数 */
} rtk_demux_stats_t;

/** 初始化参数。所有指针在实例生命周期内必须保持有效。 */
typedef struct {
    const rtk_demux_decoder_t *const *decoders; /* 解码器表，下标越小优先级越高（同首字节时先尝试） */
    uint8_t decoder_count;                      /* 1..RTK_DEMUX_MAX_DECODERS */
    uint8_t *buf;                               /* 帧累积缓冲区 */
    size_t buf_size;                            /* ≥ 所有解码器 max_frame 的最大值 */
    rtk_demux_frame_cb_t on_frame;              /* 非 NULL */
    rtk_demux_unknown_cb_t on_unknown;          /* 可为 NULL */
    void *user;                                 /* 原样传给回调 */
} rtk_demux_config_t;

/**
 * 实例状态。调用方分配（静态或栈上），字段为实现私有，只能通过本头文件的函数访问。
 * 包含 256 B 首字节位图，避免每字节轮询所有解码器。
 */
typedef struct {
    rtk_demux_config_t cfg;
    uint8_t first_mask[256]; /* bit i = decoders[i] 接受该首字节 */
    size_t len;              /* buf 中已累积的字节数 */
    size_t need;             /* 当前候选下次 step 前至少还要的字节数 */
    uint8_t active;          /* 当前候选解码器下标；无候选时为 RTK_DEMUX_MAX_DECODERS */
    uint8_t tried_mask;      /* 当前起点已拒绝过的候选位图 */
    bool in_callback;        /* 防止回调中重入 feed */
    rtk_demux_stats_t stats;
} rtk_demux_t;

typedef enum {
    RTK_DEMUX_OK = 0,
    RTK_DEMUX_ERR_ARG,     /* 参数无效：NULL、decoder_count 越界、buf_size 不足、描述符字段无效 */
    RTK_DEMUX_ERR_REENTRY, /* 在本实例回调中调用了 feed/abort/init */
} rtk_demux_err_t;

/**
 * 初始化（或重新初始化）实例：校验配置、建立首字节位图、清空状态与统计。
 * 失败时实例不可用（再次 init 成功前调用 feed 返回 RTK_DEMUX_ERR_ARG）。
 */
rtk_demux_err_t rtk_demux_init(rtk_demux_t *dmx, const rtk_demux_config_t *cfg);

/**
 * 喂入任意长度的字节（可为 0）。在本调用内同步完成所有能完成的帧回调与未知字节回调。
 * 语义与分块方式无关：同一字节序列无论如何切分喂入，产生的帧回调序列（内容、顺序）完全相同，未知字节按序拼接后
 * 也完全相同（只是 on_unknown 的切分点可能不同）。未知字节与帧之间保持流顺序：一帧回调前，先输出它之前的未知字节。
 * 失败回放：候选 REJECT 时，先在同一起点按优先级尝试其余接受该首字节的解码器；全部拒绝后，
 * 起点前移 1 字节，在已缓冲字节中寻找下一个可能的首字节并回放，不丢弃可能属于真实帧的字节。
 */
rtk_demux_err_t rtk_demux_feed(rtk_demux_t *dmx, const uint8_t *data, size_t len);

/**
 * 放弃当前半帧（例如调用方检测到流空闲超时、链路断开）：已缓冲字节按 REJECT 处理并回放重扫；
 * 回放后仍不足以成帧的剩余字节作为未知字节输出。之后实例回到空闲状态（len == 0）。
 */
rtk_demux_err_t rtk_demux_abort(rtk_demux_t *dmx);

/** 当前是否有半帧在累积（可用于调用方决定是否启动空闲超时）。 */
bool rtk_demux_busy(const rtk_demux_t *dmx);

/** 读取统计（指针在实例生命周期内有效）。 */
const rtk_demux_stats_t *rtk_demux_stats(const rtk_demux_t *dmx);

/** 清零统计，不影响解析状态。 */
void rtk_demux_reset_stats(rtk_demux_t *dmx);

#ifdef __cplusplus
}
#endif
