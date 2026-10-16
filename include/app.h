// app.h
// 应用主体：持有配置、队列和全局退出标志，并声明两个工作线程函数。
#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

#include "config.h"
#include "lockfree_queue.h"
#include "packet.h"

namespace udplog {

struct App {
    // 用显式构造函数把"配置"和"队列容量"一次定好。
    //
    // 为什么不用默认构造 + 事后赋值：队列内含 mutex/condition_variable，
    // 拷贝和移动都被禁用了（见 lockfree_queue.h 的说明），所以
    // `app.queue = LockFreeQueue<T>(n);` 这种写法编译不过。
    // 而且队列容量本来就来自配置，在构造时一次决定才是正确的设计。
    explicit App(const AppConfig& cfg)
        : config(cfg), queue(static_cast<size_t>(cfg.queue.capacity)) {}

    // 保留默认构造，容量退化为 1。仅用于不方便提供配置的场景（如单元测试）。
    App() : App(AppConfig{}) {}

    AppConfig config;

    // 有界队列：容量 = config.queue.capacity。
    // 注意成员声明顺序必须是 config 在前、queue 在后，否则 queue 的初始化
    // 会读到尚未构造的 config（C++ 按声明顺序初始化，与初始化列表顺序无关）。
    LockFreeQueue<RawPacket> queue;

    // false = 通知两个线程退出。用 atomic 是因为它被多个线程读写，
    // 且信号处理函数也会写它（信号处理函数里只能做这种"原子写"）。
    std::atomic<bool> running{true};

    // 本对象被两个线程通过指针共享，绝不能被拷贝或移动。
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
};

// 创建 UDP 监听 socket 并 bind。成功返回 fd(>=0)；失败返回 -1 并写 *err。
// 失败时 *err 里会带上可操作的原因（端口占用、权限不足、地址非法等）。
int CreateUdpSocket(const NetworkConfig& cfg, std::string* err);

// 线程1：接收 UDP 数据 -> 入队。绝不解析、绝不写磁盘。
void ReceiveThreadFunc(App* app);

// 线程2：出队 -> 解析 -> 记录日志。
void ProcessThreadFunc(App* app);

}  // namespace udplog
