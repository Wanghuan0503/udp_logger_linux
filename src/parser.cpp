// parser.cpp
// 协议解析 —— 【空实现占位】
//
// 当前 Parse() 不做任何解析，固定返回 kNotImplemented。
// 原因：MCU 协议未定，此时写任何解析逻辑都是猜测，而错误解析出的数据
// 比"明确说未实现"危险得多。
//
// ============================================================
//  协议确定后怎么填这个文件（照着做即可）
// ============================================================
// 1) 先和 MCU 方确认三件事，写进协议文档：
//      - 字节序（小端 / 大端）—— x86/ARM 都是小端，但 DSP 有例外
//      - 每个字段的偏移、宽度、含义
//      - 校验方式（CRC16 的多项式与初值？还是求和校验？）以及校验覆盖范围
//
// 2) 用协议文档里的"样例十六进制报文"当作输入，逐步填 Parse()：
//      ParseStatus PacketParser::Parse(const RawPacket& p, ParsedMessage* out) const {
//          const auto& d = p.payload;
//          if (d.size() < kHeaderLen) return ParseStatus::kTooShort;
//          if (u16(d, 0) != kMagic)   return ParseStatus::kBadMagic;
//          const uint16_t body_len = u16(d, 12);
//          if (d.size() < kHeaderLen + body_len) return ParseStatus::kBadLength;
//          if (crc16(d.data(), kHeaderLen + body_len - 2) != u16(d, kHeaderLen + body_len - 2))
//              return ParseStatus::kBadCrc;
//          out->msg_type       = d[3];
//          out->seq            = u32(d, 4);
//          out->device_time_ms = u32(d, 8);
//          out->body.assign(d.begin() + kHeaderLen, d.begin() + kHeaderLen + body_len);
//          return ParseStatus::kOk;
//      }
//
// 3) 改完后，用 tools/udp_sender 按同样布局发包自测：
//      正常包应打出 [数据] 行；把某个字节改错应打出对应的 [解析失败] 原因。
//    "能正确区分各种错误"比"能解析正常包"更能说明解析器写对了。
// ============================================================
#include "parser.h"

#include <sstream>

namespace udplog {
namespace {

// ---------------------------------------------------------------------------
// 下面这些小工具在协议确定后会立刻用上，先写好放在这里，避免到时重复造轮子。
// 全部显式按字节拼装，不直接 memcpy 到结构体 —— 结构体有填充字节，
// 且字节序依赖平台，直接 memcpy 是嵌入式解析的经典坑。
// ---------------------------------------------------------------------------

// 小端读取 uint16。调用前必须确保 data 至少有 offset+2 字节。
uint16_t u16(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

// 小端读取 uint32。
uint32_t u32(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

// 大端读取 uint16（协议若为大端则改用这两个）。
uint16_t u16be(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

// 大端读取 uint32。
uint32_t u32be(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) << 24 |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

// CRC16/CCITT-FALSE：多项式 0x1021，初值 0xFFFF。嵌入式里最常见的一种。
// 若 MCU 用的是 CRC16/MODBUS（多项式 0xA001，反射）或简单求和，替换本函数即可。
uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// 占位常量：协议确定后改成真实值（或改为从配置读取）。
// 当前未被 Parse() 使用，保留是为了让上面的示例代码可以直接落地。
[[maybe_unused]] constexpr uint16_t kMagic = 0xA55A;
[[maybe_unused]] constexpr uint8_t kVersion = 1;
[[maybe_unused]] constexpr size_t kHeaderLen = 16;

}  // namespace

const char* ParseStatusName(ParseStatus status) {
    switch (status) {
        case ParseStatus::kOk: return "OK";
        case ParseStatus::kNotImplemented: return "NOT_IMPLEMENTED";
        case ParseStatus::kTooShort: return "TOO_SHORT";
        case ParseStatus::kBadMagic: return "BAD_MAGIC";
        case ParseStatus::kBadLength: return "BAD_LENGTH";
        case ParseStatus::kBadCrc: return "BAD_CRC";
        case ParseStatus::kUnsupported: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

std::string ToHex(const std::vector<uint8_t>& data, size_t max_bytes) {
    static const char* kHex = "0123456789ABCDEF";
    const size_t count = (max_bytes > 0 && data.size() > max_bytes) ? max_bytes : data.size();
    std::string out;
    out.reserve(count * 3 + 4);
    for (size_t i = 0; i < count; ++i) {
        // 必须先转成无符号再移位：uint8_t 常被提升为 int，
        // 若直接对 char(0x80) 右移会因符号扩展得到错误结果。
        const uint8_t byte = data[i];
        out.push_back(kHex[(byte >> 4) & 0x0F]);
        out.push_back(kHex[byte & 0x0F]);
        if (i + 1 < count) {
            out.push_back(' ');
        }
    }
    if (count < data.size()) {
        out += " ...";
    }
    if (out.empty()) {
        out = "(空)";
    }
    return out;
}

std::string FormatParsed(const ParsedMessage& msg) {
    std::ostringstream oss;
    oss << "type=0x" << std::hex << static_cast<int>(msg.msg_type) << std::dec
        << " seq=" << msg.seq << " device_time_ms=" << msg.device_time_ms
        << " body_len=" << msg.body.size();
    return oss.str();
}

ParseStatus PacketParser::Parse(const RawPacket& packet, ParsedMessage* out) const {
    (void)packet;
    (void)out;
    // 协议未定：明确告知"未实现"，而不是猜一个结果。
    // 处理线程会据此走"原始报文留档"分支，不会误报成解析失败。
    return ParseStatus::kNotImplemented;
}

}  // namespace udplog
