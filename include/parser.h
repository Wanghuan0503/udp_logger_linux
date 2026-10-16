// parser.h
// 协议解析模块 —— 当前为"空实现占位"状态。
//
// 【当前阶段的设计意图】
// MCU 的协议还没定，所以解析函数是空的：收到任何报文都返回 kNotImplemented。
// 这不是缺陷，而是刻意的：
//   1. "接收线程 -> 队列 -> 处理线程 -> 日志"这条流水线可以立刻跑通并验证；
//   2. 收到的原始报文会完整写进 logs/raw/udp_raw.log，现场数据不会浪费；
//   3. 等协议定下来，只改本文件（必要时加 packet.h 的字段），
//      接收、队列、日志、配置四个模块一行都不用动。
//
// 【协议定下来之后要做的三件事】
//   1. 在 parser.cpp 里按真实字节布局填 Parse()，用那里的 u16()/u32() 等小工具读取；
//   2. 按真实字段调整 packet.h 里的 ParsedMessage；
//   3. 按需要在 ParseStatus 里增删错误分类（分类越细，现场排障越快）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "packet.h"

namespace udplog {

// 解析结果状态。分类的意义：日志里能一眼看出"是收到了别的程序的数据"
// 还是"链路有干扰导致数据损坏"，排障效率完全不同。
enum class ParseStatus {
    kOk = 0,             // 解析成功
    kNotImplemented,     // 解析逻辑尚未实现（当前阶段固定返回这个）
    kTooShort,           // 报文长度不足一个协议头
    kBadMagic,           // 帧头魔数不符（多半不是本协议的数据）
    kBadLength,          // 长度字段与实际收到的字节数矛盾
    kBadCrc,             // 校验和错误（链路干扰 / 数据损坏）
    kUnsupported,        // 协议版本不支持 / 未知命令码
};

// 状态码转可读名字，用于日志。
const char* ParseStatusName(ParseStatus status);

// 把解析结果渲染成一行 Key=Value 文本，便于写日志和后续用 awk/grep 统计。
std::string FormatParsed(const ParsedMessage& msg);

// 字节数组 -> hex 字符串。max_bytes > 0 时超出部分以 " ..." 结尾。
std::string ToHex(const std::vector<uint8_t>& data, size_t max_bytes);

class PacketParser {
public:
    // 解析一个报文。返回 kOk 时 *out 有效。
    // 被处理线程串行调用，因此不需要加锁。
    ParseStatus Parse(const RawPacket& packet, ParsedMessage* out) const;
};

}  // namespace udplog
