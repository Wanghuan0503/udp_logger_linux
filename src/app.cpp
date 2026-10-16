// app.cpp
// 应用主体：UDP 接收线程 + 队列消费线程。
//
// 【线程模型 —— 为什么必须是这两个线程】
//   * 接收线程只做最短路径的事：recvfrom -> 拷贝进队列 -> 立刻回到 recvfrom。
//     它绝不解析、绝不写磁盘。这样即使 MCU 高频发数据，内核 socket 缓冲区
//     也不容易被塞满而丢包。
//   * 处理线程做慢活：解析、拼日志、写磁盘。写磁盘可能阻塞几十毫秒，
//     放在接收线程里会造成成片丢包，所以必须分开。
//   * 队列是两者之间的缓冲带：削峰填谷。有界，所以过载时是"可控丢包"
//     而不是"内存吃爆"。
#include "app.h"

#include <arpa/inet.h>
#include <glog/logging.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"
#include "parser.h"

namespace udplog {
namespace {

// IPv4 UDP 单包理论上限，用它当接收缓冲区，保证任何包都不会被截断。
constexpr size_t kMaxUdpPayload = 65507;

// 汇报运行统计的间隔（毫秒）。
constexpr int64_t kStatsIntervalMs = 5000;

// 处理线程每次等待队列的超时（毫秒）。有数据会被立刻唤醒，所以
// 这个值只影响"完全空闲"时的唤醒频率：越大越省 CPU，越小退出越干脆。
constexpr int64_t kQueueWaitMs = 200;

// 给线程起名字，方便在 htop / ps -L 里一眼看出哪个线程在干什么。
// Linux 下有效，名字上限 16 字节（含结尾 '\0'）。
void SetThreadName(const char* name) {
#if defined(__linux__)
    (void)pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

std::string SockaddrToIp(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "?";
    }
    return std::string(buf);
}

}  // namespace

int CreateUdpSocket(const NetworkConfig& cfg, std::string* err) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        *err = std::string("socket() 失败: ") + std::strerror(errno);
        return -1;
    }

    // SO_REUSEADDR：程序重启时端口还处于残留状态也能立刻重新绑定，调试期少等。
    int reuse = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOG(WARNING) << "setsockopt(SO_REUSEADDR) 失败: " << std::strerror(errno);
    }

    // 内核接收缓冲区。注意：内核会把设置值翻倍（缓冲区记账开销），
    // 所以下面用 getsockopt 复核实际生效值，日志里留个凭证。
    const int rcvbuf = cfg.recv_buffer_bytes;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        LOG(WARNING) << "setsockopt(SO_RCVBUF) 失败: " << std::strerror(errno);
    } else {
        int actual = 0;
        socklen_t len = sizeof(actual);
        if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len) == 0) {
            LOG(INFO) << "SO_RCVBUF 请求=" << rcvbuf << "B 实际生效=" << actual << "B";
        }
    }

    // 接收超时：让 recvfrom 最多阻塞这么久，之后返回 EAGAIN。
    // 这是本工程"能优雅退出"的关键 —— 否则 recvfrom 会永远阻塞，线程无法结束。
    timeval tv{};
    tv.tv_sec = cfg.recv_timeout_ms / 1000;
    tv.tv_usec = (cfg.recv_timeout_ms % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        *err = std::string("setsockopt(SO_RCVTIMEO) 失败: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.bind_port);
    if (::inet_pton(AF_INET, cfg.bind_ip.c_str(), &addr.sin_addr) != 1) {
        *err = "inet_pton() 解析 bind_ip 失败: " + cfg.bind_ip;
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        *err = "bind(" + cfg.bind_ip + ":" + std::to_string(cfg.bind_port) +
               ") 失败: " + std::strerror(errno) +
               "\n提示：端口被占用时用 ss -lunp | grep " + std::to_string(cfg.bind_port) +
               " 查看；低于 1024 的端口需要 root 权限。";
        ::close(fd);
        return -1;
    }

    if (cfg.join_multicast) {
        ip_mreq mreq{};
        if (::inet_pton(AF_INET, cfg.multicast_group.c_str(), &mreq.imr_multiaddr) != 1) {
            *err = "组播地址非法: " + cfg.multicast_group;
            ::close(fd);
            return -1;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            *err = std::string("加入组播组失败: ") + std::strerror(errno);
            ::close(fd);
            return -1;
        }
        LOG(INFO) << "已加入组播组: " << cfg.multicast_group;
    }

    return fd;
}

void ReceiveThreadFunc(App* app) {
    SetThreadName("udp-recv");
    const NetworkConfig& cfg = app->config.network;

    std::string err;
    const int fd = CreateUdpSocket(cfg, &err);
    if (fd < 0) {
        LOG(ERROR) << "创建 UDP 监听 socket 失败: " << err;
        // 让处理线程也能退出，避免它永远卡在等待队列上。
        app->running.store(false);
        app->queue.Close();
        return;
    }
    LOG(INFO) << "UDP 监听已就绪: " << cfg.bind_ip << ":" << cfg.bind_port
              << " (recv_timeout=" << cfg.recv_timeout_ms << "ms)";

    // 只分配一次，循环复用。不要在循环里反复分配内存。
    std::vector<uint8_t> buffer(kMaxUdpPayload);

    uint64_t total = 0;        // recvfrom 成功读出的报文总数（应用层看到的量）
    uint64_t dropped = 0;      // 因队列满被丢弃的数量
    uint64_t zero_len = 0;     // 长度为 0 的数据报数量
    uint64_t recv_errors = 0;  // recvfrom 返回致命错误的次数
    uint64_t peak_depth = 0;   // 队列深度峰值：判断"离丢包还有多远"的关键指标
    int64_t last_report = NowMs();
    int64_t last_packet_ms = NowMs();  // 上一个包到达的时刻，用来说明"空闲多久了"

    // 本汇报周期内"队列达到高水位"的证据。
    //
    // 【为什么不能用汇报那一刻的瞬时深度来判断】
    // 曾经的做法是：report() 里取一次 queue.Size()，超过 75% 就告警。
    // 实测证明这个做法会漏报 —— 队列饱和时的深度会稳定在"丢一点、收一点"的
    // 平衡点上（实测约 8/16 = 50%），而峰值可能冲到 15/16。
    // 于是出现"已丢弃 34653 个包，却一条预警都没有"的荒唐结果。
    //
    // 现在改成记录"整段时间内是否曾经越线"：
    //   high_water_seen 一旦置位，就保留到下一次汇报，不会漏。
    //   TryPush 失败更直接 —— 丢包说明队列必然满过（100%），铁证。
    bool high_water_seen = false;
    uint64_t dropped_in_period = 0;  // 本周期丢弃数，用于让告警带上严重程度
    uint64_t depth_check_counter = 0;  // 用于把深度采样降频到"每 64 个包一次"

    const size_t queue_capacity = app->queue.Capacity();
    const size_t high_water = queue_capacity * 3 / 4;  // 75% 阈值

    // 统一的统计输出函数。三处调用：周期上报、空闲上报、退出汇总。
    // 单独抽出来的原因：之前统计代码只写在"收到包"的分支里，
    // 导致"一个包都没收到"时日志完全静默 —— 恰恰是最需要信息的时候。
    auto report = [&](const char* tag) {
        const size_t depth = app->queue.Size();
        peak_depth = std::max(peak_depth, static_cast<uint64_t>(depth));
        last_report = NowMs();
        LOG(INFO) << tag << " 累计收到=" << total << " 累计丢弃=" << dropped
                  << " 零长包=" << zero_len << " recv错误=" << recv_errors
                  << " 队列深度=" << depth << "/" << queue_capacity
                  << " 峰值深度=" << peak_depth;

        // 基于"整段是否越线"，而不是"此刻的瞬时值" —— 这样才不会漏报。
        if (high_water_seen && high_water > 0) {
            LOG(WARNING) << "队列达到过 " << peak_depth << "/" << queue_capacity << "（>=75%"
                         << "，本周期丢弃 " << dropped_in_period << " 个），"
                         << "消费速度跟不上生产速度。再持续下去会持续丢包，"
                         << "可考虑：关闭 parser.raw_log_enabled、把逐包日志改为抽样、"
                         << "或加大 queue.capacity。";
        }
        high_water_seen = false;
        dropped_in_period = 0;
    };

    while (app->running.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const ssize_t n = ::recvfrom(fd, buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // 接收超时或被信号打断：正常流程。
                //
                // 【重要】这里也必须检查是否该汇报统计。原先只有"收到包"的分支才汇报，
                // 结果是：MCU 一个包都没发时，日志里一条记录都没有，现场无法区分
                // "MCU 没发" 和 "程序没收"。现在空闲也照常汇报。
                if (NowMs() - last_report >= kStatsIntervalMs) {
                    const int64_t idle_ms = NowMs() - last_packet_ms;
                    std::string tag = "[接收统计·空闲]";
                    if (total == 0) {
                        // 从启动到现在一个包都没收到 —— 这是最需要被看见的状态。
                        tag += " (自启动以来未收到任何数据, 已等待 " + std::to_string(idle_ms) +
                               "ms, 请检查 MCU 是否在发送 / bind_ip 与端口是否正确 / 防火墙)";
                    } else {
                        tag += " (距上一个包 " + std::to_string(idle_ms) + "ms)";
                    }
                    report(tag.c_str());
                }
                continue;
            }
            // 其他错误（如网卡被关闭）也要能被发现，但不要刷屏。
            ++recv_errors;
            LOG_EVERY_N(ERROR, 20) << "recvfrom() 出错: " << std::strerror(errno)
                                   << " 累计错误=" << recv_errors;
            continue;
        }
        if (n == 0) {
            // UDP 允许长度为 0 的数据报。
            ++zero_len;
            LOG_EVERY_N(WARNING, 100) << "收到长度为 0 的数据报，来源: " << SockaddrToIp(peer)
                                      << " 累计=" << zero_len;
            continue;
        }

        // 组装"一个报文"：内容 + 来源 + 接收时刻。
        RawPacket packet;
        packet.payload.assign(buffer.begin(), buffer.begin() + n);
        packet.src_ip = SockaddrToIp(peer);
        packet.src_port = ntohs(peer.sin_port);
        packet.recv_time_ms = NowMs();
        last_packet_ms = packet.recv_time_ms;

        if (app->queue.TryPush(packet)) {
            ++total;
            // 深度采样：不能每个包都调 Size()（要抢锁，会拖慢收包路径，
            // 而收包路径的延迟直接决定内核缓冲区会不会溢出丢包）。
            // 所以每 64 个包采一次，用来捕捉"接近满但还没丢包"的状态。
            //
            // 注意最严重的情况不依赖这个采样：TryPush 一旦失败就说明队列
            // 必然满过（下面的 else 分支直接判定 100%），零开销、零漏报。
            ++depth_check_counter;
            if ((depth_check_counter & 0x3F) == 0) {
                const size_t depth = app->queue.Size();
                if (depth > peak_depth) {
                    peak_depth = depth;
                }
                if (high_water > 0 && depth >= high_water) {
                    high_water_seen = true;
                }
            }
        } else {
            // 入队失败 ⇒ 队列此刻必然是满的（100%）。
            // 这是"已经过载"的**铁证**，不需要再查深度，零额外开销。
            ++dropped;
            ++dropped_in_period;
            peak_depth = std::max(peak_depth, static_cast<uint64_t>(queue_capacity));
            high_water_seen = true;
            // 丢包必须能被看到，但不能刷爆日志：100 次只记 1 次。
            LOG_EVERY_N(WARNING, 100) << "队列已满，丢弃报文。累计收到=" << total
                                      << " 累计丢弃=" << dropped;
        }

        // 定时汇报（在收到包的路径上）。
        if (NowMs() - last_report >= kStatsIntervalMs) {
            report("[接收统计]");
        }
    }

    // 退出前打印最终汇总：任何时长的运行都会留下完整记录，
    // 不再依赖"恰好跨过一个 5 秒周期"。
    const int64_t idle_ms = NowMs() - last_packet_ms;
    LOG(INFO) << "[接收线程退出] 累计收到=" << total << " 累计丢弃=" << dropped
              << " 零长包=" << zero_len << " recv错误=" << recv_errors
              << " 队列残留=" << app->queue.Size() << "/" << app->queue.Capacity()
              << " 峰值深度=" << peak_depth
              << "（距上一个包 " << idle_ms << "ms）";
    if (total > 0) {
        LOG(INFO) << "[接收线程退出] 丢包率="
                  << (100.0 * static_cast<double>(dropped) /
                      static_cast<double>(total + dropped))
                  << "%（注意：这不等于端到端丢包率）";
    }
    if (dropped > 0) {
        LOG(WARNING) << "[接收线程退出] 有丢包。请用同样的发送速率跑一次"
                     << "至少 30 秒的稳态测试再判断，短测试会放大丢包率。"
                     << "若确认是持续过载，可考虑：关闭 parser.raw_log_enabled、"
                     << "把逐包日志改为抽样、或加大 queue.capacity。";
    }
    ::close(fd);
    app->queue.Close();  // 兜底唤醒处理线程（Close 幂等）
}

void ProcessThreadFunc(App* app) {
    SetThreadName("udp-parse");
    const ParserConfig& parser_cfg = app->config.parser;
    const PacketParser parser;

    uint64_t processed = 0;    // 处理过的报文总数
    uint64_t parse_failed = 0; // 解析失败数
    uint64_t peak_depth = 0;   // 消费侧看到的队列深度峰值
    int64_t last_report = NowMs();
    int64_t last_data_ms = NowMs();  // 上一个被处理的报文的时刻

    // 与接收线程同样的思路：统计抽成函数，空闲时也汇报。
    // 注意 "距上一个包 Xms" 是个很有用的诊断字段：它持续变大就说明数据停了。
    auto report = [&](const char* tag) {
        const size_t depth = app->queue.Size();
        peak_depth = std::max(peak_depth, static_cast<uint64_t>(depth));
        last_report = NowMs();
        LOG(INFO) << tag << " 累计处理=" << processed << " 解析失败=" << parse_failed
                  << " 队列深度=" << depth << "/" << app->queue.Capacity()
                  << " 峰值深度=" << peak_depth
                  << " (距上一个包 " << (NowMs() - last_data_ms) << "ms)";
    };

    // 退出条件完全交给队列的关闭语义：
    //   接收线程退出时会调用 queue.Close()，此后 WaitPop 在"队列已排空"时返回 kClosed。
    // 这样处理线程一定会把队列里的剩余数据全部处理完才退出，不会丢已收到的数据。
    RawPacket packet;
    for (;;) {
        // 固定用短超时等待。
        //
        // 为什么不用"距下次汇报的剩余时间"当超时：那样一旦某个包的处理耗时超过
        // 汇报间隔，剩余时间就变成 1ms，于是每处理一个包都会触发一次汇报，
        // 日志会被瞬间刷爆。固定短超时 + 下面按间隔节流，才是稳的。
        //
        // 有数据时会被 notify_one 立刻唤醒，所以这个超时只在"真的空闲"时生效，
        // 不会拖慢吞吐。
        const auto result = app->queue.WaitPop(&packet, kQueueWaitMs);
        if (result == LockFreeQueue<RawPacket>::WaitResult::kTimeout) {
            // 到点才汇报，避免 1ms 超时导致刷屏。report() 会重置 last_report，
            // 所以空闲期间大约每 5 秒有一条记录。
            if (NowMs() - last_report >= kStatsIntervalMs) {
                report("[处理统计·空闲]");
            }
            continue;
        }
        if (result == LockFreeQueue<RawPacket>::WaitResult::kClosed) {
            break;  // 队列已关闭且已排空，可以退出了
        }

        // 兜底退出条件：正常情况下靠上面的 kClosed 退出；
        // 但如果接收线程因异常路径没能调用 Close()，这里保证不会永远转下去。
        if (!app->running.load() && app->queue.Size() == 0) {
            break;
        }

        ++processed;
        last_data_ms = NowMs();
        const std::string src = packet.src_ip + ":" + std::to_string(packet.src_port);

        // ---------- (1) 原始报文留档 ----------
        // 协议未定阶段这一步价值最高：完整保留原始数据，将来可离线重新解析。
        if (parser_cfg.raw_log_enabled) {
            const std::string line = FormatTimeMs(packet.recv_time_ms) + " src=" + src +
                                     " len=" + std::to_string(packet.payload.size()) +
                                     " hex=" + ToHex(packet.payload, 0);
            AppendRawLog(parser_cfg, line);
        }

        // ---------- (2) 协议解析 ----------
        ParsedMessage msg;
        const ParseStatus status = parser.Parse(packet, &msg);

        if (status == ParseStatus::kNotImplemented) {
            // 协议尚未确定：这不是错误，用 INFO 记一条"收到数据"即可，
            // 不能记成 WARNING，否则会把"未实现"误判成"数据有问题"。
            LOG(INFO) << "[原始报文] src=" << src
                      << " recv_time=" << FormatTimeMs(packet.recv_time_ms)
                      << " len=" << packet.payload.size() << " hex="
                      << ToHex(packet.payload,
                               static_cast<size_t>(parser_cfg.max_payload_hex_bytes));
        } else if (status != ParseStatus::kOk) {
            // 真正的解析失败：收到了预期外的数据。用 WARNING 并降频。
            ++parse_failed;
            LOG_EVERY_N(WARNING, 20) << "[解析失败] reason=" << ParseStatusName(status)
                                     << " src=" << src << " len=" << packet.payload.size()
                                     << " hex="
                                     << ToHex(packet.payload,
                                              static_cast<size_t>(parser_cfg.max_payload_hex_bytes))
                                     << " 累计失败=" << parse_failed;
        } else {
            // 解析成功：Key=Value 格式，便于以后用 awk/grep 做统计。
            LOG(INFO) << "[数据] src=" << src
                      << " recv_time=" << FormatTimeMs(packet.recv_time_ms) << " "
                      << FormatParsed(msg) << " body_hex="
                      << ToHex(msg.body, static_cast<size_t>(parser_cfg.max_payload_hex_bytes));
        }

        // 处理过程中也要检查是否该汇报：高负载下 WaitPop 一直能拿到数据，
        // 不会走到上面的超时分支，所以这里必须再查一次。
        if (NowMs() - last_report >= kStatsIntervalMs) {
            report("[处理统计]");
        }
    }

    // 退出汇总：任何时长的运行都有记录。
    LOG(INFO) << "[处理线程退出] 累计处理=" << processed << " 解析失败=" << parse_failed
              << " 队列残留=" << app->queue.Size() << " 峰值深度=" << peak_depth;
}

}  // namespace udplog
