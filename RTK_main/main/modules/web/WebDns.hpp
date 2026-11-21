// Wi-Fi 配置模式强制门户 DNS 报文编解码（纯 C++20，不依赖 IDF/lwIP）。
// 由 WebModule 在 kLink 任务轮询调用，不阻塞、不分配堆，线程/ISR 安全。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace app::web_cfg {

constexpr size_t kDnsMaxPacketBytes = 512U; // B，RFC 1035 UDP DNS 最大报文长度
constexpr uint16_t kDnsPort = 53U;          // UDP 端口
constexpr uint32_t kDnsAnswerTtlSec = 60U;  // s，强制门户 A 记录 TTL

/**
 * @brief 解析单条 DNS 查询并构造强制门户应答报文（A 回指定 IPv4，其他类型回 NOERROR 空应答）
 * @param req: 原始 UDP 查询报文（长度超 kDnsMaxPacketBytes 或畸形均返回 0）
 * @param ipv4: A 记录应答的 IPv4 地址四字节（网络字节序）
 * @param[out] out: 应答输出缓冲（长度须 >= kDnsMaxPacketBytes）；返回 0 时内容未定义
 * @retval >0 合法查询，返回写入 out 的应答字节数
 * @retval 0 非法、截断、超长、非标准查询或包含压缩指针，调用方应静默丢弃
 * @note 纯函数，不阻塞、不分配堆，线程/ISR 安全。
 */
[[nodiscard]] size_t buildDnsReply(std::span<const uint8_t> req, std::array<uint8_t, 4U> ipv4,
                                   std::span<uint8_t> out) noexcept;

} // namespace app::web_cfg
