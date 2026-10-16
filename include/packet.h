// packet.h
// 线程间传递的"一个 UDP 报文"的载体，以及解析结果的数据结构。
//
// 协议未定，所以这里只定义"数据怎么承载"，不定义"字节怎么排布"。
// 协议确定后，主要调整 ParsedMessage 的字段（RawPacket 一般不用改）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace udplog {

// 接收线程收到的原始数据报 + 必要的元信息。
struct RawPacket {
    // 报文字节。必须用 vector<uint8_t> 而不是 std::string：
    // std::string 很多操作按 C 字符串处理，遇到字节 0x00 会被截断，
    // 而 UDP 是二进制协议，0x00 是完全合法的数据。
    std::vector<uint8_t> payload;

    // 数据来源。多台 MCU 或现场有干扰源时，这是定位问题的第一线索。
    std::string src_ip;
    uint16_t src_port = 0;

    // 控制器的接收时刻（毫秒时间戳）。
    // 为什么必须记：MCU 与控制器时钟几乎不可能同步，
    // 必须同时保留"设备时间"和"接收时间"，事后才能对齐分析。
    int64_t recv_time_ms = 0;
};

// 解析成功后的业务字段。
// TODO(协议确定后): 按真实协议增删字段。当前只有最通用的几个占位字段。
struct ParsedMessage {
    uint8_t msg_type = 0;          // 报文类型 / 命令码
    uint32_t seq = 0;              // 序号：用于检测丢包与乱序
    uint64_t device_time_ms = 0;   // MCU 自己带的时间戳（如果协议里有）
    std::vector<uint8_t> body;     // 去掉协议头之后的业务数据
};

}  // namespace udplog
