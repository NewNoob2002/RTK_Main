// 解析浏览器请求帧：单层 JSON 对象、固定字段、无堆（docs/web/ws-protocol.md §3）。
// parseRequest 由网页模块任务调用；不阻塞，非 ISR 安全；本文件不分配堆。
#include "web_proto/Request.hpp"

namespace web {
namespace {

constexpr uint32_t kMinRequestId = 1;
constexpr uint32_t kMaxRequestId = 65535;
constexpr uint32_t kDecimalBase = 10U;
constexpr uint32_t kPositiveInt32Limit = 2147483647U;
constexpr uint32_t kNegativeInt32Limit = 2147483648U;
// 最短成员为 `"a":0,`（6 字节），故 1024 B 帧的成员数有上界；用它给重复检查定界，
// 保证未知扩展字段不会因数量被拒绝（协议 §1：未知键忽略）。
constexpr size_t kMinMemberBytes = 6;
constexpr size_t kMaxMembers = kMaxFrameBytes / kMinMemberBytes + 1;

// UTF-8 编码边界（RFC 3629）：首字节区间与续字节区间。
constexpr unsigned char kAsciiMax = 0x7FU;
constexpr unsigned char kUtf8ContinuationLow = 0x80U;
constexpr unsigned char kUtf8ContinuationHigh = 0xBFU;
constexpr unsigned char kTwoByteLeadLow = 0xC2U; // 0xC0/0xC1 是过长编码
constexpr unsigned char kTwoByteLeadHigh = 0xDFU;
constexpr unsigned char kThreeByteLeadLow = 0xE0U;
constexpr unsigned char kThreeByteLeadHigh = 0xEFU;
constexpr unsigned char kFourByteLeadLow = 0xF0U;
constexpr unsigned char kFourByteLeadHigh = 0xF4U; // 0xF5 起超过 U+10FFFF
constexpr unsigned char kSurrogateLead = 0xEDU;    // 排除 U+D800..U+DFFF
constexpr unsigned char kSecondByteLowA0 = 0xA0U;
constexpr unsigned char kSecondByteHigh9F = 0x9FU;
constexpr unsigned char kSecondByteLow90 = 0x90U;
constexpr unsigned char kSecondByteHigh8F = 0x8FU;
constexpr size_t kUtf8ExtraOne = 1;
constexpr size_t kUtf8ExtraTwo = 2;
constexpr size_t kUtf8ExtraThree = 3;

/**
 * @brief  判断字符是否为 JSON 允许的空白。
 * @param  c: 待判断字符。
 * @retval true  空格、制表符、CR 或 LF
 * @retval false 其余字符
 */
bool isJsonSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/**
 * @brief  判断字符是否为十进制数字。
 * @param  c: 待判断字符。
 * @retval true  '0'..'9'
 * @retval false 其余字符
 */
bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/**
 * @brief  判断字符是否属于协议 §1 的成员名字符集。
 * @param  c: 待判断字符。
 * @retval true  小写字母、数字、`.` 或 `_`
 * @retval false 其余字符（大写、空白、转义符、非 ASCII）
 */
bool isNameChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
}

/**
 * @brief  判断字节是否为 UTF-8 续字节（0x80..0xBF）。
 * @param  c: 待判断字节。
 * @retval true  续字节
 * @retval false 其余字节
 */
bool isUtf8Continuation(char c) noexcept
{
    const auto byte = static_cast<unsigned char>(c);
    return byte >= kUtf8ContinuationLow && byte <= kUtf8ContinuationHigh;
}

/**
 * @brief  按 UTF-8 首字节确定后续字节数与首续字节的允许区间。
 * @param  lead: 首字节（>= 0x80）。
 * @param[out] extra: 后续续字节数（1..3）；非法首字节时不写。
 * @param[out] low:  首续字节下界（默认 0x80，E0/F0 抬高）。
 * @param[out] high: 首续字节上界（默认 0xBF，ED/F4 压低）。
 * @retval true  合法首字节
 * @retval false 孤立续字节、0xC0/0xC1（过长编码）或 0xF5 以上（超过 U+10FFFF）
 */
bool classifyUtf8Lead(unsigned char lead, size_t &extra, unsigned char &low,
                      unsigned char &high) noexcept
{
    low = kUtf8ContinuationLow;
    high = kUtf8ContinuationHigh;
    if (lead >= kTwoByteLeadLow && lead <= kTwoByteLeadHigh) {
        extra = kUtf8ExtraOne;
        return true;
    }
    if (lead >= kThreeByteLeadLow && lead <= kThreeByteLeadHigh) {
        extra = kUtf8ExtraTwo;
        if (lead == kThreeByteLeadLow) {
            low = kSecondByteLowA0;
        }
        if (lead == kSurrogateLead) {
            high = kSecondByteHigh9F;
        }
        return true;
    }
    if (lead >= kFourByteLeadLow && lead <= kFourByteLeadHigh) {
        extra = kUtf8ExtraThree;
        if (lead == kFourByteLeadLow) {
            low = kSecondByteLow90;
        }
        if (lead == kFourByteLeadHigh) {
            high = kSecondByteHigh8F;
        }
        return true;
    }
    return false;
}

/**
 * @brief  校验从 pos 开始的整条多字节 UTF-8 序列。
 * @param  text:  整帧字节。
 * @param  pos:   首字节偏移（< text.size()）。
 * @param  extra: 后续续字节数。
 * @param  low:   首续字节下界。
 * @param  high:  首续字节上界。
 * @retval true  序列完整且编码合法
 * @retval false 截断、首续字节越界或后续续字节非法
 */
bool isValidUtf8Sequence(std::span<const char> text, size_t pos, size_t extra, unsigned char low,
                         unsigned char high) noexcept
{
    if (text.size() - pos - 1 < extra) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text[pos + 1]);
    if (first < low || first > high) {
        return false;
    }
    for (size_t i = 2; i <= extra; ++i) {
        if (!isUtf8Continuation(text[pos + i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief  校验整段字节是否为合法 UTF-8（协议 §1：帧为 UTF-8 文本）。
 * @param  text: 待校验字节。
 * @retval true  合法（含纯 ASCII）
 * @retval false 截断、过长编码、代理区码点或超过 U+10FFFF
 * @note   纯扫描，不分配堆；O(n)。
 */
bool isValidUtf8(std::span<const char> text) noexcept
{
    size_t pos = 0;
    while (pos < text.size()) {
        const auto lead = static_cast<unsigned char>(text[pos]);
        if (lead <= kAsciiMax) {
            ++pos;
            continue;
        }
        size_t extra = 0;
        unsigned char low = kUtf8ContinuationLow;
        unsigned char high = kUtf8ContinuationHigh;
        if (!classifyUtf8Lead(lead, extra, low, high) ||
            !isValidUtf8Sequence(text, pos, extra, low, high)) {
            return false;
        }
        pos += extra + 1;
    }
    return true;
}

/**
 * @brief  判断整数原文是否在 int32 范围内（协议 §1：整数限 int32）。
 * @param  token: parseNumberToken 返回的数字原文（已含可选负号，无前导零）。
 * @retval true  可表示为 int32
 * @retval false 两侧越界
 * @note   不写输出；"-0" 视为 0（JSON 合法，未知字段无需特殊对待）。
 */
bool isInt32Text(std::string_view token) noexcept
{
    size_t pos = 0;
    bool negative = false;
    if (token.front() == '-') {
        negative = true;
        pos = 1;
    }
    const uint32_t limit = negative ? kNegativeInt32Limit : kPositiveInt32Limit;
    uint32_t magnitude = 0;
    for (; pos < token.size(); ++pos) {
        // parseNumberToken 只产出数字，digit 范围 0..9。
        const auto digit = static_cast<uint32_t>(token[pos] - '0');
        if (magnitude > (limit - digit) / kDecimalBase) {
            return false;
        }
        magnitude = magnitude * kDecimalBase + digit;
    }
    return true;
}

} // namespace

/**
 * @brief  单帧解析器：一次扫描，字段视图指向输入帧或 Request 的解码缓冲。
 * @note   只由 parseRequest 在网页模块任务内创建；不保存跨调用状态。
 */
class Parser final {
public:
    /**
     * @brief  绑定输入帧与输出请求。
     * @param  frame: 待解析文本；调用期间必须保持有效且不变。
     * @param[out] out: 输出请求；解析期间由本对象独占写入。
     */
    Parser(std::span<const char> frame, Request &out) noexcept : text_(frame), out_(&out) {}

    /**
     * @brief  解析整帧并校验字段组合。
     * @retval kNone     成功；out 的字段有效
     * @retval kTooLong  帧超过 kMaxFrameBytes
     * @retval kBadJson  非法 UTF-8、语法/结构/字段类型/缺字段/未知 t/重复键/非法成员名
     * @retval kBadValue id 不在 1..65535
     * @note   任何失败都清空 out（含 kTooLong），避免调用方误用上一条请求的视图；不阻塞、不分配堆。
     */
    [[nodiscard]] ErrorCode parse() noexcept
    {
        resetOut();
        if (text_.size() > kMaxFrameBytes) {
            return ErrorCode::kTooLong;
        }
        // 先校验整体编码，后面的字符串扫描即可假定字节流是合法 UTF-8。
        if (!isValidUtf8(text_)) {
            return ErrorCode::kBadJson;
        }
        const ErrorCode err = parseFrame();
        if (err != ErrorCode::kNone) {
            resetOut();
        }
        return err;
    }

private:
    /**
     * @brief  解析 `{...}` 与可选尾随空白。
     * @retval kNone    结构完整
     * @retval kBadJson 结构或成员名错误
     * @note   内部方法；仅 parse 调用。
     */
    [[nodiscard]] ErrorCode parseFrame() noexcept
    {
        skipSpace();
        if (!consume('{')) {
            return ErrorCode::kBadJson;
        }
        skipSpace();
        if (!consume('}')) {
            while (true) {
                std::string_view name{};
                const size_t nameStart = pos_;
                if (!parseString(name) || !isProtocolName(name)) {
                    return ErrorCode::kBadJson;
                }
                skipSpace();
                if (!consume(':')) {
                    return ErrorCode::kBadJson;
                }
                if (!rememberName(name, nameStart)) {
                    return ErrorCode::kBadJson;
                }
                skipSpace();
                const ErrorCode err = parseMember(name);
                if (err != ErrorCode::kNone) {
                    return err;
                }
                skipSpace();
                if (consume(',')) {
                    skipSpace();
                    continue;
                }
                if (consume('}')) {
                    break;
                }
                return ErrorCode::kBadJson;
            }
        }
        skipSpace();
        if (pos_ != text_.size()) {
            return ErrorCode::kBadJson;
        }
        return finishRequest();
    }

    /**
     * @brief  解析一个成员名对应的值。
     * @param  name: 成员名原文（不含引号）。
     * @retval kNone     已知字段已解析，或未知字段已跳过
     * @retval kBadJson  类型不符、重复键或值语法错误
     * @retval kBadValue id 越界
     */
    [[nodiscard]] ErrorCode parseMember(std::string_view name) noexcept
    {
        if (name == "t") {
            return parseType();
        }
        if (name == "id") {
            return parseId();
        }
        if (name == "key") {
            return parseTextField(hasKey_, out_->key);
        }
        if (name == "value") {
            return parseTextField(hasValue_, out_->value);
        }
        if (name == "name") {
            return parseTextField(hasName_, out_->name);
        }
        if (name == "token") {
            return parseTextField(hasToken_, out_->token);
        }
        if (!skipValue()) {
            return ErrorCode::kBadJson;
        }
        return ErrorCode::kNone;
    }

    /**
     * @brief  解析 `t` 字段并识别请求类型。
     * @retval kNone    已知类型
     * @retval kBadJson 非字符串、未知类型或重复字段
     */
    [[nodiscard]] ErrorCode parseType() noexcept
    {
        if (hasT_) {
            return ErrorCode::kBadJson;
        }
        std::string_view raw{};
        std::string_view text{};
        if (!parseString(raw) || !decode(raw, text)) {
            return ErrorCode::kBadJson;
        }
        if (text == "set") {
            out_->type = RequestType::kSet;
        } else if (text == "cmd") {
            out_->type = RequestType::kCmd;
        } else if (text == "unlock") {
            out_->type = RequestType::kUnlock;
        } else {
            return ErrorCode::kBadJson;
        }
        hasT_ = true;
        return ErrorCode::kNone;
    }

    /**
     * @brief  解析 `id` 字段（1..65535 的 JSON 整数）。
     * @retval kNone     范围内
     * @retval kBadJson  不是整数、语法不完整（如 `01`、`-01`）或紧跟非法字符
     * @retval kBadValue 负值、0 或超过 65535
     * @note   先确认数字语法完整，再按字段范围分类，使 `01` 与 `-01` 落到同一错误。
     */
    [[nodiscard]] ErrorCode parseId() noexcept
    {
        if (hasId_) {
            return ErrorCode::kBadJson;
        }
        std::string_view token{};
        if (!parseNumberToken(token)) {
            return ErrorCode::kBadJson;
        }
        // 数字后必须是分隔符/空白；否则是 "01"、"1x" 这类非法 JSON（前导零）。
        if (!atValueEnd()) {
            return ErrorCode::kBadJson;
        }
        if (token.front() == '-') {
            return ErrorCode::kBadValue;
        }
        uint32_t value = 0;
        for (const char c : token) {
            // 仅当字符是 '0'..'9' 时进入本函数；digit 范围 0..9。
            const uint32_t digit = static_cast<uint32_t>(c - '0');
            if (value > (kMaxRequestId - digit) / kDecimalBase) {
                return ErrorCode::kBadValue;
            }
            value = value * kDecimalBase + digit;
        }
        if (value < kMinRequestId) {
            return ErrorCode::kBadValue;
        }
        // 上面已保证 1..65535，窄化到 uint16_t 安全。
        out_->id = static_cast<uint16_t>(value);
        hasId_ = true;
        return ErrorCode::kNone;
    }

    /**
     * @brief  解析一个字符串字段并记录已出现。
     * @param  seen: 该字段是否已出现的标志；成功后置 true。
     * @param[out] field: 输出视图；成功时指向输入帧或解码缓冲。
     * @retval kNone    成功
     * @retval kBadJson 非字符串、重复字段或转义错误
     */
    [[nodiscard]] ErrorCode parseTextField(bool &seen, std::string_view &field) noexcept
    {
        if (seen) {
            return ErrorCode::kBadJson;
        }
        std::string_view raw{};
        std::string_view text{};
        if (!parseString(raw) || !decode(raw, text)) {
            return ErrorCode::kBadJson;
        }
        field = text;
        seen = true;
        return ErrorCode::kNone;
    }

    /**
     * @brief  跳过未知字段的标量值（字符串、int32 整数、true/false）。
     * @retval true  语法正确的标量已消费
     * @retval false 对象、数组、null、浮点、越界整数或语法错误
     */
    [[nodiscard]] bool skipValue() noexcept
    {
        if (pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        if (c == '"') {
            std::string_view raw{};
            return parseString(raw);
        }
        if (c == '-' || isDigit(c)) {
            std::string_view token{};
            return parseNumberToken(token) && isInt32Text(token);
        }
        return consumeLiteral("true") || consumeLiteral("false");
    }

    /**
     * @brief  校验组合字段并确认请求完整。
     * @retval kNone    类型与必需字段齐全
     * @retval kBadJson 缺 t/id 或缺少类型对应的字段
     */
    [[nodiscard]] ErrorCode finishRequest() noexcept
    {
        if (!hasT_ || !hasId_) {
            return ErrorCode::kBadJson;
        }
        switch (out_->type) {
        case RequestType::kSet:
            return hasKey_ && hasValue_ ? ErrorCode::kNone : ErrorCode::kBadJson;
        case RequestType::kCmd:
            return hasName_ ? ErrorCode::kNone : ErrorCode::kBadJson;
        case RequestType::kUnlock:
            return hasToken_ ? ErrorCode::kNone : ErrorCode::kBadJson;
        default:
            return ErrorCode::kBadJson;
        }
    }

    /**
     * @brief  判断成员名是否属于协议 §1 的字符集（小写 ASCII、数字、`.`、`_`，非空）。
     * @param  name: 成员名原文（不含引号）。
     * @retval true  合法
     * @retval false 空名、大写、转义符、空白或非 ASCII
     * @note   成员名不允许转义，因此重复比较可以直接用原文。
     */
    [[nodiscard]] bool isProtocolName(std::string_view name) const noexcept
    {
        if (name.empty()) {
            return false;
        }
        for (const char c : name) {
            if (!isNameChar(c)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief  记录已见成员名并拒绝重复。
     * @param  name:      成员名原文。
     * @param  nameStart: 该成员名左引号在帧内的偏移。
     * @retval true  首次出现且表未满
     * @retval false 重复或成员过多
     * @note   只存偏移，容量由帧长上界给出（kMaxMembers），因此正常帧不可能因成员过多失败。
     */
    [[nodiscard]] bool rememberName(std::string_view name, size_t nameStart) noexcept
    {
        for (size_t i = 0; i < nameCount_; ++i) {
            const auto start = static_cast<size_t>(nameStart_[i]);
            if (slice(start, name.size()) == name) {
                return false;
            }
        }
        if (nameCount_ >= nameStart_.size()) {
            return false;
        }
        // nameStart + 1 <= kMaxFrameBytes，可放入 uint16_t。
        nameStart_[nameCount_] = static_cast<uint16_t>(nameStart + 1);
        ++nameCount_;
        return true;
    }

    /**
     * @brief  解析带引号的 JSON 字符串，返回引号之间的原文。
     * @param[out] raw: 输出原文（未解码转义）。
     * @retval true  找到结束引号且转义合法
     * @retval false 未闭合、非法转义或含裸控制字符
     * @note   只接受 `\"` `\\` `\/` `\b` `\f` `\n` `\r` `\t`；不支持 `\uXXXX`。
     */
    [[nodiscard]] bool parseString(std::string_view &raw) noexcept
    {
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return false;
        }
        ++pos_;
        const size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') {
                raw = slice(start, pos_ - start);
                ++pos_;
                return true;
            }
            if (c == '\\') {
                if (pos_ + 1 >= text_.size()) {
                    return false;
                }
                const char escaped = text_[pos_ + 1];
                if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' &&
                    escaped != 'f' && escaped != 'n' && escaped != 'r' && escaped != 't') {
                    return false;
                }
                pos_ += 2;
                continue;
            }
            // 裸控制字符在 JSON 字符串中非法。
            if (static_cast<unsigned char>(c) < 0x20U) {
                return false;
            }
            ++pos_;
        }
        return false;
    }

    /**
     * @brief  解析 JSON 整数文本，拒绝小数与指数形式。
     * @param[out] token: 输出数字原文（含可选负号）。
     * @retval true  语法正确且已消费
     * @retval false 前导零、缺少数字、浮点或非法起始字符
     * @note   不限制数值大小，由调用方按字段范围检查。
     */
    [[nodiscard]] bool parseNumberToken(std::string_view &token) noexcept
    {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size()) {
            return false;
        }
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && isDigit(text_[pos_])) {
                ++pos_;
            }
        } else {
            return false;
        }
        if (pos_ < text_.size() &&
            (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            return false;
        }
        token = slice(start, pos_ - start);
        return true;
    }

    /**
     * @brief  判断数字之后是否为合法分隔（空白、`,`、`}` 或帧尾）。
     * @retval true  数字语法完整
     * @retval false 紧跟其他字符（`01`、`1x` 等）
     */
    [[nodiscard]] bool atValueEnd() const noexcept
    {
        if (pos_ >= text_.size()) {
            return true;
        }
        const char c = text_[pos_];
        return isJsonSpace(c) || c == ',' || c == '}';
    }

    /**
     * @brief  把原文中的转义解码到 Request 的解码缓冲。
     * @param  raw:   parseString 返回的原文。
     * @param[out] text: 输出视图；无转义时直接指向 raw（即输入帧），有转义时指向内部缓冲。
     * @retval true  解码完成
     * @retval false 解码缓冲不足或转义非法
     */
    [[nodiscard]] bool decode(std::string_view raw, std::string_view &text) noexcept
    {
        text = raw;
        if (raw.find('\\') == std::string_view::npos) {
            return true;
        }
        const size_t begin = out_->decodedSize_;
        for (size_t i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '\\') {
                ++i;
                if (i >= raw.size()) {
                    return false;
                }
                switch (raw[i]) {
                case '"':
                    c = '"';
                    break;
                case '\\':
                    c = '\\';
                    break;
                case '/':
                    c = '/';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                default:
                    return false;
                }
            }
            if (out_->decodedSize_ >= Request::kDecodedCapacity) {
                return false;
            }
            out_->decoded_[out_->decodedSize_] = c;
            ++out_->decodedSize_;
        }
        // begin 与 decodedSize_ 都在缓冲容量内，直接取子视图，不走会抛异常的 substr。
        const std::span<const char> decoded(out_->decoded_.data(), out_->decoded_.size());
        const std::span<const char> part = decoded.subspan(begin, out_->decodedSize_ - begin);
        text = std::string_view(part.data(), part.size());
        return true;
    }

    /**
     * @brief  跳过空白字符。
     * @note   不阻塞；不越界。
     */
    void skipSpace() noexcept
    {
        while (pos_ < text_.size() && isJsonSpace(text_[pos_])) {
            ++pos_;
        }
    }

    /**
     * @brief  若当前字符匹配则消费。
     * @param  c: 期望字符。
     * @retval true  已消费
     * @retval false 不匹配或已到末尾
     */
    [[nodiscard]] bool consume(char c) noexcept
    {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    /**
     * @brief  取输入帧的一段视图。
     * @param  start:  起始偏移（调用方保证在帧内）。
     * @param  length: 字节数（调用方保证 start + length <= 帧长）。
     * @retval 指向输入帧的视图
     * @note   不拷贝、不分配堆；使用指针加长度构造，避免 substr 的越界异常路径。
     */
    [[nodiscard]] std::string_view slice(size_t start, size_t length) const noexcept
    {
        const std::span<const char> part = text_.subspan(start, length);
        return std::string_view(part.data(), part.size());
    }

    /**
     * @brief  若剩余文本以给定字面量开头则消费。
     * @param  literal: 期望字面量。
     * @retval true  已消费
     * @retval false 不匹配或剩余不足
     */
    [[nodiscard]] bool consumeLiteral(std::string_view literal) noexcept
    {
        if (!matchLiteral(literal)) {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    /**
     * @brief  判断剩余文本是否以给定字面量开头。
     * @param  literal: 字面量。
     * @retval true  匹配
     * @retval false 不匹配或剩余不足
     */
    [[nodiscard]] bool matchLiteral(std::string_view literal) const noexcept
    {
        if (text_.size() - pos_ < literal.size()) {
            return false;
        }
        return slice(pos_, literal.size()) == literal;
    }

    /**
     * @brief  清空输出请求的字段与解码缓冲。
     * @note   不释放资源；使失败时调用方看不到半成品视图。
     */
    void resetOut() noexcept
    {
        out_->type = RequestType::kSet;
        out_->id = 0;
        out_->key = {};
        out_->value = {};
        out_->name = {};
        out_->token = {};
        out_->decodedSize_ = 0;
    }

    std::span<const char> text_;
    Request *out_;
    size_t pos_ = 0;
    // 只保存成员名前引号之后的内容偏移；成员名无转义，可据此直接比较原文。
    std::array<uint16_t, kMaxMembers> nameStart_{};
    size_t nameCount_ = 0;
    bool hasT_ = false;
    bool hasId_ = false;
    bool hasKey_ = false;
    bool hasValue_ = false;
    bool hasName_ = false;
    bool hasToken_ = false;
};

ErrorCode parseRequest(std::span<const char> frame, Request &out) noexcept
{
    Parser parser(frame, out);
    return parser.parse();
}

} // namespace web
