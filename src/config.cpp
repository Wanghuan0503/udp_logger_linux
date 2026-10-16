// config.cpp
// 用 yaml-cpp 读取 config.yaml。
//
// 设计要点：
//   1. 每个字段都有默认值。配置文件缺字段时用默认值，不会崩。
//   2. 关键字段（IP、端口、容量、级别）读后做合法性校验，出错直接报错退出。
//      这能避免"监听在错误端口上跑了一整天"这类事故。
//   3. 错误信息写清"哪个字段错了、错在哪"，现场排障成本主要由它决定。
#include "config.h"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <sstream>

namespace udplog {
namespace {

// 读标量的小工具：键不存在时保留默认值。
// 注意不要写成 out = node[key].as<T>()：键不存在时 yaml-cpp 会抛异常。
template <typename T>
void ReadScalar(const YAML::Node& node, const char* key, T* out) {
    if (node && node[key]) {
        *out = node[key].as<T>();
    }
}

// 校验 IPv4 点分十进制字符串。只做格式检查，足以拦住手误。
bool IsValidIpv4(const std::string& ip) {
    int part_count = 0;
    size_t start = 0;
    while (start <= ip.size()) {
        const size_t end = ip.find('.', start);
        const std::string part =
            ip.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part.size() > 3) {
            return false;
        }
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        const int value = std::atoi(part.c_str());
        if (value < 0 || value > 255) {
            return false;
        }
        ++part_count;
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return part_count == 4;
}

// 级别字符串 -> glog 数值（0=INFO 1=WARNING 2=ERROR 3=FATAL）。
// 这里只做校验，实际设置发生在 logger.cpp。
bool IsValidLevel(const std::string& level) {
    return level == "INFO" || level == "info" || level == "WARNING" || level == "WARN" ||
           level == "warning" || level == "ERROR" || level == "error" || level == "FATAL" ||
           level == "fatal";
}

}  // namespace

bool LoadConfig(const std::string& path, AppConfig* out, std::string* err) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::BadFile&) {
        // 最常见的原因：相对路径基于"当前运行目录"，而不是可执行文件所在目录。
        *err = "配置文件不存在或无法打开: " + path +
               "\n提示：相对路径是基于当前运行目录解析的，可改用 -f 指定绝对路径。";
        return false;
    } catch (const YAML::Exception& e) {
        *err = std::string("YAML 解析失败: ") + e.what() + "\n文件: " + path +
               "\n提示：YAML 只能用空格缩进，不能用 Tab。";
        return false;
    }

    if (!root.IsMap()) {
        *err = "配置文件顶层必须是 key: value 映射结构: " + path;
        return false;
    }

    try {
        const YAML::Node net = root["network"];
        ReadScalar(net, "bind_ip", &out->network.bind_ip);
        ReadScalar(net, "bind_port", &out->network.bind_port);
        ReadScalar(net, "recv_buffer_bytes", &out->network.recv_buffer_bytes);
        ReadScalar(net, "recv_timeout_ms", &out->network.recv_timeout_ms);
        ReadScalar(net, "join_multicast", &out->network.join_multicast);
        ReadScalar(net, "multicast_group", &out->network.multicast_group);

        const YAML::Node q = root["queue"];
        ReadScalar(q, "capacity", &out->queue.capacity);
        ReadScalar(q, "full_policy", &out->queue.full_policy);

        const YAML::Node p = root["parser"];
        ReadScalar(p, "raw_log_enabled", &out->parser.raw_log_enabled);
        ReadScalar(p, "raw_log_file", &out->parser.raw_log_file);
        ReadScalar(p, "max_payload_hex_bytes", &out->parser.max_payload_hex_bytes);

        const YAML::Node lg = root["log"];
        ReadScalar(lg, "level", &out->log.level);
        ReadScalar(lg, "also_log_to_stderr", &out->log.also_log_to_stderr);
        ReadScalar(lg, "dir", &out->log.dir);
        ReadScalar(lg, "file_prefix", &out->log.file_prefix);
        ReadScalar(lg, "max_size_mb", &out->log.max_size_mb);
        ReadScalar(lg, "max_files", &out->log.max_files);
    } catch (const YAML::Exception& e) {
        // 典型触发：端口写成了带引号的字符串、布尔值写成了别的词。
        *err = std::string("配置字段类型错误: ") + e.what() +
               "\n提示：数字不要加引号，布尔值只能是 true / false。";
        return false;
    }

    // ---------------- 合法性校验 ----------------
    if (!IsValidIpv4(out->network.bind_ip)) {
        *err = "network.bind_ip 不是合法 IPv4 地址: \"" + out->network.bind_ip +
               "\"\n提示：监听所有网卡请写 \"0.0.0.0\"。";
        return false;
    }
    if (out->network.bind_port == 0) {
        *err = "network.bind_port 必须大于 0（建议 1024~65535，低于 1024 需要 root 权限）";
        return false;
    }
    if (out->network.recv_buffer_bytes < 512) {
        *err = "network.recv_buffer_bytes 太小（至少 512），建议 65536";
        return false;
    }
    if (out->network.recv_timeout_ms < 10) {
        *err = "network.recv_timeout_ms 太小（至少 10 毫秒），否则接收线程会空转烧 CPU";
        return false;
    }
    if (out->queue.capacity < 1) {
        *err = "queue.capacity 必须 >= 1";
        return false;
    }
    if (out->queue.full_policy != "drop_new" && out->queue.full_policy != "drop_old") {
        *err = "queue.full_policy 只能是 drop_new 或 drop_old，当前值: " + out->queue.full_policy;
        return false;
    }
    if (out->log.dir.empty()) {
        *err = "log.dir 不能为空";
        return false;
    }
    if (!IsValidLevel(out->log.level)) {
        *err = "log.level 只能是 INFO / WARNING / ERROR / FATAL，当前值: " + out->log.level;
        return false;
    }
    return true;
}

std::string DumpConfig(const AppConfig& cfg) {
    std::ostringstream oss;
    oss << "\n  [network] bind=" << cfg.network.bind_ip << ":" << cfg.network.bind_port
        << " recv_buffer=" << cfg.network.recv_buffer_bytes
        << " recv_timeout=" << cfg.network.recv_timeout_ms << "ms"
        << " multicast=" << (cfg.network.join_multicast ? "on" : "off")
        << "\n  [queue]   capacity=" << cfg.queue.capacity << " full_policy=" << cfg.queue.full_policy
        << "\n  [parser]  raw_log=" << (cfg.parser.raw_log_enabled ? "on" : "off")
        << " file=" << cfg.parser.raw_log_file
        << " max_hex=" << cfg.parser.max_payload_hex_bytes << "B"
        << "\n  [log]     level=" << cfg.log.level << " dir=" << cfg.log.dir
        << " prefix=" << cfg.log.file_prefix << " stderr=" << (cfg.log.also_log_to_stderr ? "on" : "off")
        << " max_size=" << cfg.log.max_size_mb << "MB max_files=" << cfg.log.max_files;
    return oss.str();
}

}  // namespace udplog
