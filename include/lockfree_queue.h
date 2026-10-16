// lockfree_queue.h
// 线程间数据交接队列（生产者 = 接收线程，消费者 = 处理线程）。
//
// 名字说明：这里用的是"互斥量 + 环形缓冲"的有界队列，不是真正的无锁结构，
// 名字沿用工程习惯叫法。选它的理由：
//   1. 逻辑简单，零基础也能看懂，不容易写出偶发的数据竞争 bug；
//   2. 有界 —— 内存占用可控，MCU 高速灌包时不会把控制器内存吃爆；
//   3. 支持 Close()，让消费线程在关机时能退出阻塞等待；
//   4. 用模板封装，将来若真需要换成无锁实现，调用方不用改。
//
// 本项目的负载下（每秒最多几千个包），mutex 的开销完全可忽略。
// "用最不容易出错的方案"在项目早期是正确的工程决策。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace udplog {

template <typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity)
        : buffer_(capacity < 1 ? 1 : capacity), capacity_(buffer_.size()) {}

    // 【重要】拷贝和移动全部显式禁用。
    //
    // 为什么：队列内含 std::mutex 与 std::condition_variable（这两者本身不可拷贝
    // 也不可移动），而且队列是"有身份的运行时对象"—— 一旦接收/处理线程已经持有
    // 它的引用，再把它复制或搬走，线程就会操作一个"孤儿队列"。
    // 所以正确做法不是事后给队列赋值，而是让容量在构造时一次定好。
    //
    // 踩坑记录：若这里只写 `operator=(const LockFreeQueue&) = delete;`，
    // 会连同"移动赋值"一起被抑制（用户声明拷贝赋值会阻止移动赋值的隐式生成），
    // 于是 `app.queue = LockFreeQueue<T>(n);` 会报：
    //     error: use of deleted function
    //     'udplog::LockFreeQueue<T>& udplog::LockFreeQueue<T>::operator=(const ...)'
    // 正确做法见 include/app.h 里的 App 构造函数。
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    // 生产者调用：尝试入队。
    // 返回 false 表示队列已满（数据被丢弃）或队列已关闭，两种情况都不阻塞。
    // 注意：这里刻意不做"满了就等"的阻塞行为 —— 接收线程宁可丢包也不能被卡住。
    bool TryPush(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || size_ == capacity_) {
            return false;
        }
        buffer_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        ++size_;
        not_empty_.notify_one();
        return true;
    }

    // 消费者调用：取一条数据。
    //
    // 返回值有三态，用 WaitResult 区分（不能用 bool，因为"超时"和"已关闭"
    // 需要不同处理：超时要汇报空闲统计，已关闭要退出线程）：
    //   kGotItem  -> 拿到数据，*out 有效
    //   kTimeout  -> 等待超时且没数据（未关闭），调用方可借此做周期任务
    //   kClosed   -> 队列已关闭且已排空，消费者应退出
    //
    // wait 使用"谓词形式"是必须的：条件变量存在虚假唤醒，
    // 被唤醒不代表条件已经成立，必须重新判断。
    enum class WaitResult { kGotItem, kTimeout, kClosed };

    // timeout_ms 传入毫秒数；< 0 表示无限等待（默认行为）。
    WaitResult WaitPop(T* out, int64_t timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this] { return closed_ || size_ > 0; };

        if (timeout_ms < 0) {
            not_empty_.wait(lock, ready);
        } else if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            // 超时且条件不成立。注意要复查 closed_：可能刚好在超时那一刻被关闭。
            return closed_ ? WaitResult::kClosed : WaitResult::kTimeout;
        }

        if (size_ == 0) {
            return WaitResult::kClosed;  // 只有 closed_ 为真时才可能走到这里
        }
        *out = buffer_[head_];
        buffer_[head_] = T();  // 及时释放元素可能持有的内存（例如 vector 的缓冲区）
        head_ = (head_ + 1) % capacity_;
        --size_;
        return WaitResult::kGotItem;
    }

    // 关机流程：唤醒所有等待者，并让消费者在处理完剩余数据后退出。
    // 必须幂等（可重复调用），因为接收线程退出时也可能调用一次做兜底。
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
    }

    // 以下只读查询用于监控与打日志，可被任意线程随时调用。
    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    size_t Capacity() const { return capacity_; }

    bool Closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::vector<T> buffer_;   // 环形缓冲本体
    size_t capacity_ = 0;
    size_t head_ = 0;         // 消费者位置
    size_t tail_ = 0;         // 生产者位置
    size_t size_ = 0;         // 当前元素个数
    bool closed_ = false;     // Close() 后被置为 true
};

}  // namespace udplog
