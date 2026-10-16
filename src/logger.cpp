// logger.cpp
// glog 初始化 + 裸日志文件写入 + 日志文件自动清理。
//
// 【glog 关键概念（零基础必读）】
//   * 四个级别：INFO / WARNING / ERROR / FATAL。FATAL 会直接终止进程。
//   * 默认行为：INFO 只写文件；WARNING 及以上同时打到 stderr。
//   * logtostderr 与 alsologtostderr 的区别（很容易搞错）：
//       - FLAGS_logtostderr = 1   ：只打终端，**不生成日志文件**
//       - FLAGS_alsologtostderr = 1：终端和文件**都要**
//     本项目要在终端看的同时保留文件，所以用后者。
//   * 每个进程每个级别一个文件，文件名形如：
//       <前缀>.<级别>.<主机名>.<用户名>.log.<日期>-<时间>.<pid>
//     并生成软链接 <前缀>.<级别> 指向最新文件，可 tail -f。
//   * glog 不会自动创建日志目录，必须自己先建好。
//   * InitGoogleLogging 必须在任何 LOG() 之前调用，整个进程只调一次。
//   * glog 会轮转（单文件超过 max_log_size 就换新文件），但**不会自动删除旧文件**。
//     所以本文件提供 PruneOldLogs()，启动时清理超出保留数量的历史文件。
#include "logger.h"

#include <glog/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace udplog {
namespace {

std::mutex g_raw_log_mutex;

// 级别字符串 -> glog 数值常量。
int LevelToGlogThreshold(const std::string& level) {
    if (level == "WARNING" || level == "WARN" || level == "warning") return google::GLOG_WARNING;
    if (level == "ERROR" || level == "error") return google::GLOG_ERROR;
    if (level == "FATAL" || level == "fatal") return google::GLOG_FATAL;
    return google::GLOG_INFO;
}

// 确保某个文件所在目录存在；没有目录部分时视为当前目录。
bool EnsureParentDir(const std::string& file_path, std::string* err) {
    const std::filesystem::path p(file_path);
    const std::filesystem::path dir = p.parent_path();
    if (dir.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        *err = "无法创建目录 " + dir.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// 清理旧的日志文件：每个级别只保留最新 keep 个。
// 为什么要自己写：glog 只管轮转、不管清理，长期运行会把磁盘写满。
// 按"文件名里的时间戳 + pid"排序近似等价于按时间排序（同一个 pid 下成立）。
void PruneOldLogs(const std::string& dir, const std::string& prefix, int keep) {
    if (prefix.empty() || keep <= 0) {
        return;
    }
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // 只处理 glog 生成的文件，绝不动用户放在同一目录里的其他文件。
        // 不匹配形如 "<prefix>.INFO." 的普通文件；软链接 <prefix>.INFO 因为
        // 不是 regular_file 也已被上面的判断排除。
        if (name.rfind(prefix + ".", 0) != 0 || name.find(".log.") == std::string::npos) {
            continue;
        }
        files.push_back(entry);
    }
    if (ec || files.empty()) {
        return;
    }

    // 按"级别的标识（WARNING/ERROR/...）+ 文件名"分组排序，再逐组裁剪。
    const char* kLevels[] = {"INFO", "WARNING", "ERROR", "FATAL"};
    for (const char* level : kLevels) {
        const std::string tag = std::string(".") + level + ".";
        std::vector<std::filesystem::directory_entry> group;
        for (const auto& entry : files) {
            if (entry.path().filename().string().find(tag) != std::string::npos) {
                group.push_back(entry);
            }
        }
        if (static_cast<int>(group.size()) <= keep) {
            continue;
        }
        std::sort(group.begin(), group.end(),
                  [](const std::filesystem::directory_entry& a,
                     const std::filesystem::directory_entry& b) {
                      return a.path().filename().string() < b.path().filename().string();
                  });
        const int remove_count = static_cast<int>(group.size()) - keep;
        for (int i = 0; i < remove_count; ++i) {
            std::error_code rm_ec;
            std::filesystem::remove(group[static_cast<size_t>(i)].path(), rm_ec);
            if (!rm_ec) {
                LOG(INFO) << "清理旧日志文件: " << group[static_cast<size_t>(i)].path().filename().string();
            }
        }
    }
}

}  // namespace

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string FormatTimeMs(int64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    const int millis = static_cast<int>(ms % 1000);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &seconds);
#else
    localtime_r(&seconds, &tm_buf);  // 线程安全版本，不能用 localtime
#endif
    char buf[64] = {0};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  millis);
    return std::string(buf);
}

bool InitLogging(const LogConfig& cfg, const std::string& argv0, std::string* err) {
    // glog 不建目录，必须自己建。
    std::error_code ec;
    std::filesystem::create_directories(cfg.dir, ec);
    if (ec) {
        *err = "无法创建日志目录 " + cfg.dir + ": " + ec.message();
        return false;
    }

    // 程序名只取文件名部分，否则 glog 生成的文件名里会带路径分隔符。
    std::string program = std::filesystem::path(argv0).filename().string();
    if (program.empty()) {
        program = "udp_logger";
    }
    google::InitGoogleLogging(program.c_str());

    // ---- 输出位置 ----
    FLAGS_log_dir = cfg.dir;                             // 日志目录（末尾不要带斜杠）
    FLAGS_logtostderr = 0;                               // 保持 0：我们始终要文件
    FLAGS_alsologtostderr = cfg.also_log_to_stderr ? 1 : 0;  // 1 = 同时打到终端
    if (cfg.also_log_to_stderr) {
        FLAGS_stderrthreshold = google::GLOG_INFO;       // 终端显示 INFO 及以上
    }
    FLAGS_minloglevel = LevelToGlogThreshold(cfg.level);  // 写入文件的最低级别
    FLAGS_log_prefix = 1;                                // 每行带 时间/线程/文件:行号

    // ---- 文件名前缀与软链接 ----
    if (!cfg.file_prefix.empty()) {
        const std::string base = cfg.dir + "/" + cfg.file_prefix;
        // SetLogDestination 的参数是"目录 + 文件名前缀"（末尾带点），
        // glog 会在后面追加上 级别 + 主机名 + 用户名 + 时间 + pid。
        google::SetLogDestination(google::GLOG_INFO, (base + ".INFO.").c_str());
        google::SetLogDestination(google::GLOG_WARNING, (base + ".WARNING.").c_str());
        google::SetLogDestination(google::GLOG_ERROR, (base + ".ERROR.").c_str());
        google::SetLogDestination(google::GLOG_FATAL, (base + ".FATAL.").c_str());

        // 软链接用 FLAGS_log_link 来设，只给"前缀"。
        //
        // 【踩过的坑】这里原本用的是 google::SetLogSymlink(GLOG_INFO, "udp_logger.INFO")，
        // 结果生成了 udp_logger.INFO.INFO —— 因为 SetLogSymlink 的参数同样只是"前缀"，
        // 它会在后面自动补上 ".INFO"。而 FLAGS_log_link 是"链接名基底"，glog 会
        // 自动追加 "$LEVEL" 后再建链，正好得到我们想要的 udp_logger.INFO。
        FLAGS_log_link = cfg.file_prefix;
    }

    // ---- 轮转与可靠性 ----
    FLAGS_max_log_size = cfg.max_size_mb > 0 ? cfg.max_size_mb : 100;  // 单文件上限（MB）
    FLAGS_logbufsecs = 0;                  // 0 = 每条日志立即 flush，崩溃也不丢日志
    FLAGS_stop_logging_if_full_disk = 1;   // 磁盘满时停止写文件，而不是把程序拖死
    // 说明：日志降频（同一行别刷屏）不靠 flag，而靠调用点的 LOG_EVERY_N 等宏，
    // 见 app.cpp 里的用法。glog 内建的同内容去重也会自动生效。

    // ---- 启动横幅 ----
    LOG(INFO) << "================ udp_logger 启动 ================";
    LOG(INFO) << "glog 版本: " << google::VersionString();
    LOG(INFO) << "日志目录: " << cfg.dir << " / 前缀: " << cfg.file_prefix
              << " / 文件最低级别: " << cfg.level << " / 单文件上限: " << FLAGS_max_log_size << "MB";

    // ---- 自检：软链接是否建出来了，并把"该看哪个文件"直接写进日志 ----
    // 目的：零基础阶段不用去猜文件名和软链接规则，日志自己告诉你。
    if (!cfg.file_prefix.empty()) {
        const std::filesystem::path link = std::filesystem::path(cfg.dir) / (cfg.file_prefix + ".INFO");
        std::error_code link_ec;
        const bool link_ok = std::filesystem::is_symlink(link, link_ec);
        if (link_ok) {
            LOG(INFO) << "实时查看（软链接，始终指向最新文件）: tail -f " << link.string();
        } else {
            LOG(WARNING) << "软链接未生成: " << link.string()
                         << "（某些文件系统/权限设置下会这样）。"
                         << "可直接查看日志目录里最新的 " << cfg.file_prefix
                         << ".INFO.* 文件，或执行：ls -t " << cfg.dir << "/" << cfg.file_prefix
                         << ".INFO.* | head -1";
        }
    }

    // ---- 清理历史日志（glog 自己不做这件事）----
    PruneOldLogs(cfg.dir, cfg.file_prefix, cfg.max_files);
    return true;
}

void AppendRawLog(const ParserConfig& cfg, const std::string& line) {
    if (!cfg.raw_log_enabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_raw_log_mutex);

    // 每次写入前顺带确认目录存在（幂等、开销可忽略；即使 logs 被删也能自愈）。
    std::string err;
    if (!EnsureParentDir(cfg.raw_log_file, &err)) {
        LOG_EVERY_N(ERROR, 100) << err;
        return;
    }

    // 追加模式打开。注意：这里每次都开关文件，是为了在崩溃时也最大程度保住数据。
    // 若将来写入频率极高（每秒上万条），再改成常开文件 + 定时 flush。
    std::ofstream ofs(cfg.raw_log_file, std::ios::app | std::ios::binary);
    if (!ofs) {
        LOG_EVERY_N(ERROR, 100) << "无法打开裸日志文件: " << cfg.raw_log_file;
        return;
    }
    ofs << line << '\n';
    if (!ofs.good()) {
        LOG_EVERY_N(ERROR, 100) << "写入裸日志文件失败（磁盘满或权限问题?）: " << cfg.raw_log_file;
    }
}

}  // namespace udplog
