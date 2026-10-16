// logger.h
// 日志模块：对 glog 做一层薄封装。
// 好处：业务代码不直接依赖 glog 宏，将来换日志库只改这一层。
#pragma once

#include <cstdint>
#include <string>

#include "config.h"

namespace udplog {

// 根据配置初始化 glog：日志目录、文件名前缀、级别、轮转参数。
// argv0 用于给 glog 一个程序名（影响默认文件名）。
// 返回 false 表示初始化失败（例如日志目录无法创建），错误写入 *err。
bool InitLogging(const LogConfig& cfg, const std::string& argv0, std::string* err);

// 把"一行原始报文记录"追加写入裸日志文件（按需开启）。
// 独立文件的目的：完整保留原始数据，协议定下来后可离线复盘。
// 内置独立互斥锁（glog 不负责这个文件），可被任意线程调用。
void AppendRawLog(const ParserConfig& cfg, const std::string& line);

// 毫秒时间戳 -> "YYYY-MM-DD HH:MM:SS.mmm"（本地时区）。
std::string FormatTimeMs(int64_t ms);

// 当前系统时间（毫秒时间戳）。
int64_t NowMs();

}  // namespace udplog
