// udp_sender.cpp
// MCU 模拟器：按指定速率向指定地址发 UDP 包。
//
// 为什么必须有这个工具：没有 MCU 在手边时，它是唯一能验证"接收线程 -> 队列 ->
// 处理线程 -> 日志"整条链路的办法。协议确定后，再给它加一个"按协议打包"的模式，
// 它就能同时充当协议一致性的验证工具。
//
// 用法示例：
//   ./udp_sender                                  # 向 127.0.0.1:9000 发 10 个包
//   ./udp_sender -h 192.168.1.10 -p 9000 -n 1000  # 发 1000 个包
//   ./udp_sender -n 20000 -r 5000                 # 以 5000 包/秒发 20000 个包（压力测试）
//   ./udp_sender -s 1200                          # 每包 1200 字节（测试大包）
//   ./udp_sender -s 0                             # 每包 0 字节（测试零长包）
//   ./udp_sender -x "A5 5A 01 02"                 # 发送指定的十六进制内容（测试畸形报文）
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

void PrintUsage(const char* argv0) {
    std::cout
        << "用法: " << argv0 << " [选项]\n"
        << "选项:\n"
        << "  -h, --host <IP>     目标 IP         (默认 127.0.0.1)\n"
        << "  -p, --port <端口>   目标端口        (默认 9000)\n"
        << "  -n, --count <个数>  发送包数, 0=无限 (默认 10)\n"
        << "  -r, --rate <包/秒>  发送速率, 0=不限速(默认 0)\n"
        << "  -s, --size <字节>   每包字节数      (默认 32; 0 表示发空包)\n"
        << "  -x, --hex \"AA BB\"  发送指定的十六进制内容(会忽略 -s)\n"
        << "      --seq-offset <n> 起始序号      (默认 0)\n"
        << "      --help          显示本帮助\n"
        << "\n说明: 当前主程序的解析器是占位实现，所以任何内容都会被记录为\n"
        << "      [原始报文]。协议确定后，可给本工具加一个\"按协议打包\"的选项。\n";
}

// 把 "A5 5A 01" 这样的字符串解析成字节数组；非法字符返回 false。
bool ParseHexString(const std::string& text, std::vector<uint8_t>* out) {
    out->clear();
    int hi = -1;
    for (const char c : text) {
        if (c == ' ' || c == ',' || c == '\t' || c == '\n') {
            continue;
        }
        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else return false;

        if (hi < 0) {
            hi = value;
        } else {
            out->push_back(static_cast<uint8_t>((hi << 4) | value));
            hi = -1;
        }
    }
    return hi < 0;  // 奇数个十六进制字符视为非法
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 9000;
    long long count = 10;
    double rate = 0.0;
    int size = 32;
    bool have_hex = false;
    std::string hex_text;
    uint32_t seq_offset = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "错误: " << name << " 后面需要跟一个值\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-h" || arg == "--host") {
            host = need_value("--host");
        } else if (arg == "-p" || arg == "--port") {
            port = std::atoi(need_value("--port"));
        } else if (arg == "-n" || arg == "--count") {
            count = std::atoll(need_value("--count"));
        } else if (arg == "-r" || arg == "--rate") {
            rate = std::atof(need_value("--rate"));
        } else if (arg == "-s" || arg == "--size") {
            size = std::atoi(need_value("--size"));
        } else if (arg == "-x" || arg == "--hex") {
            hex_text = need_value("--hex");
            have_hex = true;
        } else if (arg == "--seq-offset") {
            seq_offset = static_cast<uint32_t>(std::atoll(need_value("--seq-offset")));
        } else {
            std::cerr << "错误: 未知参数 " << arg << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (port <= 0 || port > 65535) {
        std::cerr << "错误: 端口非法: " << port << "\n";
        return 2;
    }
    if (size < 0 || size > 65507) {
        std::cerr << "错误: 包大小非法(0~65507): " << size << "\n";
        return 2;
    }

    std::vector<uint8_t> fixed_payload;
    if (have_hex && !ParseHexString(hex_text, &fixed_payload)) {
        std::cerr << "错误: --hex 内容非法，应为空格分隔的十六进制字节，例如 \"A5 5A 01\"\n";
        return 2;
    }

    // 创建 socket。
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "错误: socket() 失败: " << std::strerror(errno) << "\n";
        return 1;
    }
    // 让内核在我们被调度延迟时不至于阻塞太久（发送侧一般不会，属于保险）。
    int sndbuf = 1 << 20;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
        std::cerr << "错误: 目标 IP 非法: " << host << "\n";
        ::close(fd);
        return 2;
    }

    // 预先生成负载。每个包带一点可区分的内容，便于在日志里核对是否收全。
    // 结构（简单可读，不是真实协议）： 每包开头 4 字节 = 序号(小端)，其余为随机字节。
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);

    std::vector<uint8_t> payload;
    if (have_hex) {
        payload = fixed_payload;
    } else {
        payload.resize(static_cast<size_t>(size));
    }

    const double interval_us = (rate > 0.0) ? (1e6 / rate) : 0.0;
    auto next_send = std::chrono::steady_clock::now();

    std::printf("目标 %s:%d  包数=%lld  速率=%s  大小=%s\n", host.c_str(), port, count,
                rate > 0 ? (std::to_string(rate) + " 包/秒").c_str() : "不限速",
                have_hex ? (std::to_string(payload.size()) + " 字节(固定内容)").c_str()
                         : (std::to_string(payload.size()) + " 字节").c_str());

    long long sent = 0;
    while (count == 0 || sent < count) {
        if (!have_hex && size >= 4) {
            const uint32_t seq = seq_offset + static_cast<uint32_t>(sent);
            payload[0] = static_cast<uint8_t>(seq & 0xFF);
            payload[1] = static_cast<uint8_t>((seq >> 8) & 0xFF);
            payload[2] = static_cast<uint8_t>((seq >> 16) & 0xFF);
            payload[3] = static_cast<uint8_t>((seq >> 24) & 0xFF);
            for (size_t i = 4; i < payload.size(); ++i) {
                payload[i] = static_cast<uint8_t>(dist(rng));
            }
        }

        const ssize_t n = ::sendto(fd, payload.data(), payload.size(), 0,
                                   reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (n < 0) {
            std::cerr << "sendto() 失败: " << std::strerror(errno) << " (已发送 " << sent << " 包)\n";
            std::cerr << "提示: 若报 \"Connection refused\"，说明目标端口上没有程序在监听。\n";
            ::close(fd);
            return 1;
        }
        ++sent;

        // 打印每个包的十六进制内容（只在包数不多且没限速时打印，避免刷屏）。
        if (count > 0 && count <= 20 && !have_hex && payload.size() <= 16) {
            std::printf("  第 %lld 包 (%zd 字节): ", sent, payload.size());
            for (const uint8_t b : payload) {
                std::printf("%02X ", b);
            }
            std::printf("\n");
        }

        if (interval_us > 0.0) {
            next_send += std::chrono::microseconds(static_cast<long long>(interval_us));
            std::this_thread::sleep_until(next_send);
        }
    }

    std::printf("发送完成: 共 %lld 个包\n", sent);
    ::close(fd);
    return 0;
}
