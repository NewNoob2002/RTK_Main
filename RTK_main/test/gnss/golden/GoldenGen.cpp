// 黄金向量生成器（ADR-0012 P3a 第 1 步）：用**未修改的** ADR-0010 方言接口（gnss::Dialect）导出
// 探测命令、全部 rover_step 组合、应答判定样本与能力位，写到 test/gnss/golden/Golden<Family>.inc。
// 只用一次：在基线 4cf1320 上运行，产物固化提交；ADR-0012 删除 Dialect 接口后本文件不再参与构建，
// 保留在 golden/ 下作为向量来源的凭证（不要改造成用 Receiver 重新生成，那会失去独立基线）。
// 线程/调用约束：单线程命令行工具，可阻塞写文件；不访问硬件。
// 构建方式（基线代码，P3a 前的方言开关名）：见 docs/reports/P3a.md 第 1 节命令记录。
//   cmake --build builds/p3a-golden --target golden_gen && builds/p3a-golden/golden_gen test/gnss/golden
// 堆：开发者工具，使用 std::string/std::vector，不在固件内。
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/decoders/Ubx.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "gnss/Dialect.hpp"
#include "gnss/dialects/Comnav.hpp"
#include "gnss/dialects/Ublox.hpp"
#include "gnss/dialects/Unicore.hpp"

namespace {

constexpr size_t kMaxSteps = 16;
constexpr std::array<uint8_t, 6> kRates{1, 2, 5, 10, 20, 3};
constexpr std::array<uint8_t, 3> kPorts{1, 2, 3};
constexpr std::array<uint32_t, 3> kBauds{0, 115200, 460800};
constexpr size_t kMaxFrame = 512;

/** 把字节全写成 \xNN 转义，避免不可打印字节与 C 字面量的歧义。 */
std::string escape(std::span<const uint8_t> bytes)
{
    std::string out;
    char buf[8];
    for (const uint8_t byte : bytes) {
        std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned>(byte));
        out += buf;
    }
    return out;
}

std::string quoted(std::span<const uint8_t> bytes)
{
    return "\"" + escape(bytes) + "\"";
}

/** 原始字节 → std::string（record_reply 收字符串，写文件时再统一转义）。 */
std::string as_text(std::span<const uint8_t> bytes)
{
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::span<const uint8_t> bytes_of(const std::string &text)
{
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

// ---- 与旧方言测试相同的样本构造（test/gnss/TestGnssDialect*.cpp 的副本，避免生成器依赖测试文件）----

std::string with_checksum(const std::string &body, bool include_dollar = false)
{
    unsigned checksum = 0;
    if (include_dollar) {
        checksum ^= static_cast<unsigned>('$');
    }
    for (const char c : body) {
        checksum ^= static_cast<unsigned char>(c);
    }
    char tail[8] = {};
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", checksum);
    return "$" + body + tail;
}

std::string command_reply(const std::string &echo, const std::string &text,
                          bool include_dollar = false)
{
    return with_checksum("command," + echo + ",response: " + text, include_dollar);
}

std::span<const uint8_t> as_bytes(const std::vector<uint8_t> &data)
{
    return std::span<const uint8_t>(data.data(), data.size());
}

void put_text(std::vector<uint8_t> &payload, const char *text, size_t width)
{
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    for (size_t i = 0; i < width; ++i) {
        payload.push_back(i < len ? static_cast<uint8_t>(text[i]) : 0U);
    }
}

std::vector<uint8_t> make_frame(uint8_t cls, uint8_t id, const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> frame{0xB5,
                               0x62,
                               cls,
                               id,
                               static_cast<uint8_t>(payload.size() & 0xFFU),
                               static_cast<uint8_t>((payload.size() >> 8) & 0xFFU)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    for (size_t i = 2; i < frame.size(); ++i) {
        ck_a = static_cast<uint8_t>(ck_a + frame[i]);
        ck_b = static_cast<uint8_t>(ck_b + ck_a);
    }
    frame.push_back(ck_a);
    frame.push_back(ck_b);
    return frame;
}

std::vector<uint8_t> mon_ver(const char *model, const char *fw)
{
    std::vector<uint8_t> payload;
    put_text(payload, "EXT CORE 1.00 (61b2dd)", 30);
    put_text(payload, "00180000", 10);
    std::vector<uint8_t> mod(30, 0);
    for (size_t i = 0; i < 4; ++i) {
        mod[i] = static_cast<uint8_t>("MOD="[i]);
    }
    for (size_t i = 0; model[i] != '\0' && (4 + i) < mod.size(); ++i) {
        mod[4 + i] = static_cast<uint8_t>(model[i]);
    }
    payload.insert(payload.end(), mod.begin(), mod.end());
    std::vector<uint8_t> ver(30, 0);
    for (size_t i = 0; i < 6; ++i) {
        ver[i] = static_cast<uint8_t>("FWVER="[i]);
    }
    for (size_t i = 0; fw[i] != '\0' && (6 + i) < ver.size(); ++i) {
        ver[6 + i] = static_cast<uint8_t>(fw[i]);
    }
    payload.insert(payload.end(), ver.begin(), ver.end());
    return make_frame(0x0A, 0x04, payload);
}

// ---- 生成 ----

struct StepText {
    std::string bytes;
    bool expect_reply = true;
    uint16_t timeout_ms = 0;
    uint32_t switch_baud = 0;
};

struct CaseText {
    gnss::Model model = gnss::Model::kUnknown;
    uint8_t rate_hz = 0;
    bool gsv = false;
    uint8_t port = 0;
    uint32_t port_baud = 0;
    uint16_t step_first = 0;
    uint8_t step_count = 0;
};

struct ProbeMatchText {
    std::string decoder;
    std::string frame;
    bool hit = false;
    gnss::Model model = gnss::Model::kUnknown;
    std::string fw;
};

struct ReplyText {
    std::string decoder;
    std::string frame;
    std::string sent;
    uint8_t ack = 0;
};

struct CapsText {
    gnss::Model model = gnss::Model::kUnknown;
    uint32_t caps = 0;
};

const char *ack_name(gnss::Reply reply)
{
    switch (reply) {
    case gnss::Reply::kAck:
        return "kAck";
    case gnss::Reply::kNack:
        return "kNack";
    case gnss::Reply::kNone:
    default:
        return "kNone";
    }
}

/** 文件名/命名空间用的家族标签（与 gnss::Family 一一对应，不含连字符）。 */
const char *family_tag(gnss::Family family)
{
    switch (family) {
    case gnss::Family::kUnicore:
        return "Unicore";
    case gnss::Family::kUblox:
        return "Ublox";
    case gnss::Family::kComnav:
        return "Comnav";
    case gnss::Family::kUnknown:
    default:
        return "Unknown";
    }
}

bool write_family(const gnss::Dialect &dialect, const std::string &dir)
{
    std::vector<StepText> steps;
    std::vector<CaseText> cases;
    std::vector<ProbeMatchText> probes;
    std::vector<ReplyText> replies;
    std::vector<CapsText> caps;

    gnss::CmdBuf probe_cmd;
    const bool probe_ok = dialect.build_probe(probe_cmd);

    constexpr uint8_t kLastModel = static_cast<uint8_t>(gnss::Model::kK803);
    for (uint8_t value = 0; value <= kLastModel; ++value) {
        const auto model = static_cast<gnss::Model>(value);
        if (gnss::family_of(model) != dialect.family) {
            continue;
        }
        for (const uint8_t rate : kRates) {
            for (const bool gsv : {false, true}) {
                for (const uint8_t port : kPorts) {
                    for (const uint32_t baud : kBauds) {
                        gnss::RoverConfig cfg{};
                        cfg.rate_hz = rate;
                        cfg.gsv = gsv;
                        cfg.port = port;
                        cfg.port_baud = baud;
                        CaseText item{};
                        item.model = model;
                        item.rate_hz = rate;
                        item.gsv = gsv;
                        item.port = port;
                        item.port_baud = baud;
                        // step_first 是 size_t 但测试侧按 uint16_t 记录；超出即失败（组合数远小于上限）
                        item.step_first = static_cast<uint16_t>(steps.size());
                        for (size_t index = 0; index < kMaxSteps; ++index) {
                            gnss::ConfigStep step{};
                            if (!dialect.rover_step(index, model, cfg, step)) {
                                break;
                            }
                            StepText text{};
                            text.bytes = quoted(step.cmd.view());
                            text.expect_reply = step.expect_reply;
                            text.timeout_ms = step.timeout_ms;
                            text.switch_baud = step.switch_baud;
                            steps.push_back(text);
                            ++item.step_count;
                        }
                        if (item.step_count == 0) {
                            std::fprintf(stderr, "%s: rover_step 对 %s 一步都没有生成\n",
                                         dialect.name, gnss::model_name(model));
                            return false;
                        }
                        if (item.step_count == kMaxSteps) {
                            std::fprintf(stderr, "%s: rover 脚本超过 %zu 步\n", dialect.name,
                                         kMaxSteps);
                            return false;
                        }
                        cases.push_back(item);
                    }
                }
            }
        }
        caps.push_back(CapsText{model, dialect.caps(model)});
    }
    caps.push_back(CapsText{gnss::Model::kUnknown, dialect.caps(gnss::Model::kUnknown)});

    // 应答/探测样本按家族给：沿用旧测试里出现过的真实样本与反例。
    gnss::ProbeResult result{};
    const auto record_probe = [&](std::string decoder, std::span<const uint8_t> frame,
                                  const demux::Decoder &dec) {
        const bool hit = dialect.match_probe(dec, frame, result);
        ProbeMatchText text{};
        text.decoder = std::move(decoder);
        text.frame = as_text(frame);
        text.hit = hit;
        if (hit) {
            text.model = result.model;
            text.fw = result.fw_version.data();
        }
        result = gnss::ProbeResult{};
        probes.push_back(text);
    };
    const auto record_reply = [&](std::string decoder, std::string frame, std::string sent,
                                  const demux::Decoder &dec) {
        const gnss::Reply reply = dialect.match_reply(dec, bytes_of(frame), bytes_of(sent));
        replies.push_back(ReplyText{std::move(decoder), std::move(frame), std::move(sent),
                                    static_cast<uint8_t>(reply)});
    };

    if (dialect.family == gnss::Family::kUnicore) {
        const std::string version =
            "#VERSIONA,96,GPS,FINE,2281,45803,0,0,18,10;\"UM982\",\"R4.10Build11233\",\"HRPT00-S10C-P\","
            "\"2310415000123\",\"R4.10.0\",\"ff\"*12345678";
        record_probe("kUnicoreHash", bytes_of(version), demux::unicore_hash::kDecoder);
        record_probe("kNmea", bytes_of(version), demux::nmea::kDecoder);
        record_probe("kUnicoreHash", bytes_of(version).first(32), demux::unicore_hash::kDecoder);
        const std::string um980 =
            "#VERSIONA,0,GPS,UNKNOWN,0,0,0,0,0,249;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
            "\"2310415000001-MD22A2244202679\",\"ff3b7c6f3c5fd90b\",\"2023/11/24\"*3f55fb4d";
        record_probe("kUnicoreHash", bytes_of(um980), demux::unicore_hash::kDecoder);
        const std::string other =
            "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678";
        record_probe("kUnicoreHash", bytes_of(other), demux::unicore_hash::kDecoder);

        const std::string sent = "UNLOG COM2\r\n";
        record_reply("kUnicoreResp", command_reply("UNLOG COM2", "OK"), sent,
                     gnss::unicore::kResponseDecoder);
        // UM980 R4.10 实测把 '$' 算进校验（含 '$' 的 XOR）。
        record_reply("kUnicoreResp", command_reply("VERSIONA", "OK", true), "VERSIONA\r\n",
                     gnss::unicore::kResponseDecoder);
        record_reply("kUnicoreResp", command_reply("UNLOG COM2", "ERROR"), sent,
                     gnss::unicore::kResponseDecoder);
        record_reply("kUnicoreResp", command_reply("OTHER", "OK"), sent,
                     gnss::unicore::kResponseDecoder);
        record_reply("kNmea", command_reply("UNLOG COM2", "OK"), sent, demux::nmea::kDecoder);
        record_reply("kUnicoreResp", "$command,UNLOG COM2,response: OK*", sent,
                     gnss::unicore::kResponseDecoder);
        record_reply("kUnicoreResp", command_reply("UNLOG COM2", "OK"), "UNLOG COM3\r\n",
                     gnss::unicore::kResponseDecoder);
    } else if (dialect.family == gnss::Family::kUblox) {
        const auto f9p = mon_ver("ZED-F9P", "HPG 1.32");
        record_probe("kUbx", as_bytes(f9p), demux::ubx::kDecoder);
        record_probe("kNmea", as_bytes(f9p), demux::nmea::kDecoder);
        record_probe("kUbx", as_bytes(f9p).first(20), demux::ubx::kDecoder);
        const auto f9r = mon_ver("ZED-F9R", "HPG 1.32");
        record_probe("kUbx", as_bytes(f9r), demux::ubx::kDecoder);

        const std::vector<uint8_t> ack = make_frame(0x05, 0x01, {0x06, 0x8A});
        const std::vector<uint8_t> nak = make_frame(0x05, 0x00, {0x06, 0x8A});
        const std::vector<uint8_t> ack_other = make_frame(0x05, 0x01, {0x06, 0x01});
        record_reply("kUbx", as_text(as_bytes(ack)), "", demux::ubx::kDecoder);
        record_reply("kUbx", as_text(as_bytes(nak)), "", demux::ubx::kDecoder);
        record_reply("kUbx", as_text(as_bytes(ack_other)), "", demux::ubx::kDecoder);
        record_reply("kUbx", as_text(as_bytes(ack).first(ack.size() - 1)), "",
                     demux::ubx::kDecoder);
        record_reply("kNmea", as_text(as_bytes(ack)), "", demux::nmea::kDecoder);
    } else if (dialect.family == gnss::Family::kComnav) {
        const std::string version =
            "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"K803\",\"1\",\"2\"*12345678\r\n";
        record_probe("kUnicoreHash", bytes_of(version), demux::unicore_hash::kDecoder);
        record_probe("kNmea", bytes_of(version), demux::nmea::kDecoder);
        record_probe("kUnicoreHash", bytes_of(version).first(24), demux::unicore_hash::kDecoder);
        const std::string other =
            "#VERSIONA,96,GPS,FINE,1,1,0,0,1,1;\"OEM719\",\"1\",\"2\"*12345678\r\n";
        record_probe("kUnicoreHash", bytes_of(other), demux::unicore_hash::kDecoder);

        record_reply("kComnavPrompt", "<OK\r\n", "", gnss::comnav::kPromptDecoder);
        record_reply("kComnavPrompt", "<ERROR\r\n", "", gnss::comnav::kPromptDecoder);
        record_reply("kComnavPrompt", "<ERROR: bad command\r\n", "", gnss::comnav::kPromptDecoder);
        record_reply("kComnavPrompt", "<WARNING\r\n", "", gnss::comnav::kPromptDecoder);
        record_reply("kUnicoreHash", "<OK\r\n", "", demux::unicore_hash::kDecoder);
    } else {
        std::fprintf(stderr, "未知家族\n");
        return false;
    }

    const char *tag = family_tag(dialect.family);
    const std::string path = dir + "/" + std::string("Golden") + tag + ".inc";
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "无法写 %s\n", path.c_str());
        return false;
    }
    std::fprintf(
        file,
        "// 黄金向量 %s（ADR-0012 P3a）：由 test/gnss/golden/GoldenGen.cpp 在基线 4cf1320 的\n",
        dialect.name);
    std::fprintf(file, "// ADR-0010 方言实现（%s）上导出。测试只读；不得为迁就新实现修改本文件。\n",
                 dialect.name);
    std::fprintf(file, "// 家族标签 %s。\n", tag);
    std::fprintf(file, "// 组合数：rover %zu，步骤 %zu；样本：探测 %zu，应答 %zu；能力位 %zu。\n",
                 cases.size(), steps.size(), probes.size(), replies.size(), caps.size());
    std::fprintf(file, "namespace golden_%s {\n\n", tag);

    std::fprintf(file, "inline constexpr golden::Probe kProbe{%s, %s};\n\n",
                 quoted(probe_cmd.view()).c_str(), probe_ok ? "true" : "false");

    std::fprintf(file, "inline constexpr std::array<golden::Step, %zu> kSteps{\n{\n", steps.size());
    for (const StepText &step : steps) {
        std::fprintf(file, "    {%s, %s, %u, %lu},\n", step.bytes.c_str(),
                     step.expect_reply ? "true" : "false", static_cast<unsigned>(step.timeout_ms),
                     static_cast<unsigned long>(step.switch_baud));
    }
    std::fprintf(file, "}\n};\n\n");

    std::fprintf(file, "// model / rate_hz / gsv / port / port_baud / step_first / step_count\n");
    std::fprintf(file, "inline constexpr std::array<golden::RoverCase, %zu> kRover{\n{\n",
                 cases.size());
    for (const CaseText &item : cases) {
        std::fprintf(file, "    {%u, %u, %s, %u, %lu, %u, %u}, // %s\n",
                     static_cast<unsigned>(item.model), static_cast<unsigned>(item.rate_hz),
                     item.gsv ? "true" : "false", static_cast<unsigned>(item.port),
                     static_cast<unsigned long>(item.port_baud),
                     static_cast<unsigned>(item.step_first), static_cast<unsigned>(item.step_count),
                     gnss::model_name(item.model));
    }
    std::fprintf(file, "}\n};\n\n");

    std::fprintf(file, "// decoder / frame / hit / model / fw\n");
    std::fprintf(file, "inline constexpr std::array<golden::ProbeMatchCase, %zu> kProbeMatch{\n{\n",
                 probes.size());
    for (const ProbeMatchText &item : probes) {
        std::fprintf(file, "    {golden::DecoderKind::%s, %s, %s, %u, \"%s\"},\n",
                     item.decoder.c_str(), quoted(bytes_of(item.frame)).c_str(),
                     item.hit ? "true" : "false", static_cast<unsigned>(item.model),
                     item.fw.c_str());
    }
    std::fprintf(file, "}\n};\n\n");

    std::fprintf(file, "// decoder / frame / sent / ack\n");
    std::fprintf(file, "inline constexpr std::array<golden::ReplyCase, %zu> kReply{\n{\n",
                 replies.size());
    for (const ReplyText &item : replies) {
        std::fprintf(file, "    {golden::DecoderKind::%s, %s, %s, %u}, // %s\n",
                     item.decoder.c_str(), quoted(bytes_of(item.frame)).c_str(),
                     quoted(bytes_of(item.sent)).c_str(), static_cast<unsigned>(item.ack),
                     ack_name(static_cast<gnss::Reply>(item.ack)));
    }
    std::fprintf(file, "}\n};\n\n");

    std::fprintf(file, "inline constexpr std::array<golden::CapsCase, %zu> kCaps{\n{\n",
                 caps.size());
    for (const CapsText &item : caps) {
        std::fprintf(file, "    {%u, %lu}, // %s\n", static_cast<unsigned>(item.model),
                     static_cast<unsigned long>(item.caps), gnss::model_name(item.model));
    }
    std::fprintf(file, "}\n};\n\n}  // namespace golden_%s\n", tag);

    const bool closed = std::fclose(file) == 0;
    std::printf("%s: %zu 组 rover（%zu 步），%zu 个探测样本，%zu 个应答样本\n", path.c_str(),
                cases.size(), steps.size(), probes.size(), replies.size());
    return closed;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : "golden";
    bool ok = true;
    for (const gnss::Dialect *dialect : gnss::enabled_dialects()) {
        if (dialect == nullptr) {
            continue;
        }
        ok = write_family(*dialect, dir) && ok;
    }
    return ok ? 0 : 1;
}
