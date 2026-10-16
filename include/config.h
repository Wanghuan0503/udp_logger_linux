// config.h
// 配置模块：从 YAML 文件读取运行参数。
// 设计原则：只依赖 yaml-cpp，不依赖网络/日志/线程模块，便于单独测试。
#pragma once

#include <cstdint>
#include <string>

namespace udplog {

// ---------------- 配置数据结构 ----------------
// 每个结构体对应 config.yaml 里的一个顶层小节。
// 每个字段都有默认值：配置文件里缺字段时用默认值，不会导致程序起不来。

struct NetworkConfig {
    // "0.0.0.0" = 监听本机所有网卡。先用它确认能通，再收窄到具体网卡。
    std::string bind_ip = "0.0.0.0";
    uint16_t bind_port = 9000;
    // 内核接收缓冲区（SO_RCVBUF）。调大能抵抗 MCU 突发，堆满才会丢包。
    int recv_buffer_bytes = 65536;
    // socket 接收超时（毫秒）。这是"能优雅退出"的关键：让 recvfrom 周期性返回，
    // 接收线程才有机会检查退出标志。值越小退出越干脆，但空转越多。
    int recv_timeout_ms = 200;
    // 仅当 MCU 用组播发送时才需要打开。
    bool join_multicast = false;
    std::string multicast_group = "239.0.0.1";
};

struct QueueConfig {
    // 队列最大报文数。有界是为了"过载时可控地丢包"，而不是把内存吃爆。
    int capacity = 4096;
    // "drop_new"：队列满时丢弃新数据（保证已入队的数据处理完整）。
    // "drop_old"：队列满时丢弃最旧数据（保证能看到最新状态）。
    // 当前实现按 drop_new 行为工作，drop_old 留作后续扩展。
    std::string full_policy = "drop_new";
};

struct ParserConfig {
    // 是否把完整原始报文以 hex 落盘。协议未定期强烈建议保持 true：
    // 解析逻辑将来一定会改，原始数据留档才能离线重新解析，现场测试不白跑。
    bool raw_log_enabled = true;
    // 裸报文日志文件路径（相对路径基于程序的工作目录，不是可执行文件目录）。
    std::string raw_log_file = "logs/raw/udp_raw.log";
    // 日志里最多打印多少字节的 hex，避免大包把日志撑爆。
    int max_payload_hex_bytes = 256;
};

struct LogConfig {
    // 写入文件的最低级别：INFO / WARNING / ERROR / FATAL。
    std::string level = "INFO";
    // 是否同时输出到终端（用 alsologtostderr，日志文件仍会照常生成）。
    bool also_log_to_stderr = true;
    // 日志目录，不存在时程序会自动创建（glog 自己不会建目录）。
    std::string dir = "logs";
    // 文件名前缀，最终形如 udp_logger.INFO.主机名.用户名.log.日期-时间.pid
    std::string file_prefix = "udp_logger";
    // 单个日志文件上限（MB）。0 = 使用默认值 100。
    int max_size_mb = 100;
    // 保留的历史文件个数。0 = 使用 glog 默认。
    int max_files = 20;
};

struct AppConfig {
    NetworkConfig network;
    QueueConfig queue;
    ParserConfig parser;
    LogConfig log;
};

// ---------------- 读取入口 ----------------
// 加载成功返回 true；失败返回 false 并向 *err 写入中文错误描述。
bool LoadConfig(const std::string& path, AppConfig* out, std::string* err);

// 把生效配置渲染成多行文本，启动时打进日志。
// 用途：一眼核对"程序到底读进去了什么"，避免"改的文件不是生效的文件"。
std::string DumpConfig(const AppConfig& cfg);

}  // namespace udplog
