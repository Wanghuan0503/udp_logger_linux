// main.cpp
// 程序入口：命令行 -> 读配置 -> 初始化日志 -> 启动两个线程 -> 等信号 -> 优雅退出。
//
// 本文件只负责"程序的生命周期"，业务逻辑都在 config/logger/parser/app 里。
// 这样拆分的好处：换配置格式、换协议、改日志策略，都不用动这里。
#include <glog/logging.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "app.h"
#include "config.h"
#include "logger.h"

namespace {

// 【重要】信号处理函数里能安全做的事极少：只能操作 volatile sig_atomic_t
// 或 lock-free 的 std::atomic<bool>。绝对不能在这里调用 LOG()、printf、malloc。
//
// 原因：信号可能在任意指令处打断主线程。如果那一刻主线程正持有 glog 内部锁，
// 而你在信号处理函数里又调用 LOG() 去抢同一把锁 —— 立即死锁，Ctrl+C 失效。
std::atomic<bool> g_stop_requested{false};

extern "C" void SignalHandler(int signum) {
    (void)signum;
    g_stop_requested.store(true);
}

void PrintUsage(const char* argv0) {
    std::cout << "用法: " << argv0 << " [选项]\n"
              << "选项:\n"
              << "  -f, --config <路径>   指定 YAML 配置文件 (默认: config/config.yaml)\n"
              << "  -h, --help            显示本帮助\n"
              << "\n示例:\n"
              << "  " << argv0 << " -f config/config.yaml\n"
              << "\n提示: 相对路径基于当前运行目录。建议在工程根目录下执行，\n"
              << "      或使用 ./scripts/run.sh（它会自动切到脚本所在目录的上级）。\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/config.yaml";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "-f" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "错误: " << arg << " 后面需要跟配置文件路径\n";
                PrintUsage(argv[0]);
                return 2;
            }
            config_path = argv[++i];
            continue;
        }
        std::cerr << "错误: 未知参数 " << arg << "\n";
        PrintUsage(argv[0]);
        return 2;
    }

    // ---------- 1. 读配置（必须在日志之前，因为日志参数来自配置）----------
    udplog::AppConfig config;
    std::string err;
    if (!udplog::LoadConfig(config_path, &config, &err)) {
        std::cerr << "配置加载失败:\n" << err << "\n";
        return 1;
    }

    // ---------- 2. 初始化日志 ----------
    if (!udplog::InitLogging(config.log, argv[0], &err)) {
        std::cerr << "日志初始化失败:\n" << err << "\n";
        return 1;
    }

    // ---------- 3. 注册信号，实现 Ctrl+C / kill 优雅退出 ----------
    struct sigaction sa {};
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);   // Ctrl+C
    ::sigaction(SIGTERM, &sa, nullptr);  // kill / systemctl stop
    ::signal(SIGPIPE, SIG_IGN);          // 对端不可达时别让进程被 SIGPIPE 干掉

    LOG(INFO) << "配置文件: " << config_path;
    LOG(INFO) << "生效配置:" << udplog::DumpConfig(config);
    LOG(INFO) << "协议解析状态: 未实现（占位）—— 报文将只做原始留档，"
              << "协议确定后只需修改 src/parser.cpp";

    // ---------- 4. 组装应用对象 ----------
    // 队列容量来自配置，所以在构造 App 时一次定好（队列不可拷贝/移动，
    // 不能"先默认构造再赋值"，见 include/app.h 的注释）。
    udplog::App app(config);

    // ---------- 5. 启动两个工作线程 ----------
    // 线程1: 收包入队；线程2: 出队解析写日志。
    std::thread recv_thread(udplog::ReceiveThreadFunc, &app);
    std::thread proc_thread(udplog::ProcessThreadFunc, &app);

    LOG(INFO) << "两个工作线程已启动 (udp-recv / udp-parse)，主线程等待退出信号";

    // ---------- 6. 主线程：等待退出信号 ----------
    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG(INFO) << "收到退出信号，开始优雅关闭……";

    // ---------- 7. 关机时序（顺序很重要）----------
    // (a) 通知接收线程退出。它最多在 recv_timeout_ms 后从 recvfrom 超时返回并发现标志。
    app.running.store(false);
    // (b) 立刻唤醒可能正在等待队列的处理线程，并告知"不会再有新数据"。
    //     注意：处理线程仍会把队列里剩余的数据处理完再退出，不会丢已收到的数据。
    app.queue.Close();

    // (c) 等待两个线程真正结束。必须 join，否则退出时日志可能来不及写。
    recv_thread.join();
    proc_thread.join();

    LOG(INFO) << "所有线程已退出，程序正常结束";
    google::ShutdownGoogleLogging();  // 刷盘，否则最后几行日志可能丢
    return 0;
}
