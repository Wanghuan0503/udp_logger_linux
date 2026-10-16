# CMakeLists.txt
# 构建配置。零基础的话，只需要知道：
#   1. cmake -S . -B build   ：生成构建文件（"根据这个描述生成 Makefile"）
#   2. cmake --build build   ：执行编译
# 依赖都来自系统包（apt 安装），不需要手动下载源码。
#
# 本文件要点：
#   * 把除 main.cpp 之外的源文件编成一个静态库 udp_core，main.cpp 和测试工具
#     分别链接它。这样测试工具可以复用同样的解析/日志代码，不用重复编译。
#   * -pthread 是必须的（std::thread）。CMake 里用 find_package(Threads) 处理。
#   * yaml-cpp 的 CMake 目标名是小写的带连字符的 yaml-cpp::yaml-cpp，
#     写成 yaml-cpp 或 YamlCpp 都会报 "target not found"。
cmake_minimum_required(VERSION 3.16)
project(udp_logger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)          # 需要 std::filesystem
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# 默认输出到 build/ 下，方便脚本定位。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------- 依赖 ----------------
find_package(Threads REQUIRED)      # std::thread / std::mutex / condition_variable
find_package(glog REQUIRED)         # 日志库
find_package(yaml-cpp REQUIRED)     # 配置解析库

message(STATUS "glog 找到:      ${glog_INCLUDE_DIRS}")
message(STATUS "yaml-cpp 找到:  ${yaml-cpp_INCLUDE_DIRS}")
message(STATUS "构建类型:       ${CMAKE_BUILD_TYPE}")

# ---------------- 公共编译选项 ----------------
# 注意：这里刻意不开 -Wpedantic。-Wall -Wextra 已经能覆盖绝大多数真实问题，
# 而 -Wpedantic 会在第三方库头文件（glog / yaml-cpp）里刷出大量无关警告，
# 反而掩盖我们自己代码里的真问题。将来如果警告变多，再按需升级。
add_library(udp_options INTERFACE)
target_compile_options(udp_options INTERFACE
    -Wall -Wextra              # 警告不是报错，但多数警告都是真 bug
    -Wno-unused-parameter
)
target_compile_definitions(udp_options INTERFACE
    _GNU_SOURCE                # pthread_setname_np 需要
)

# ---------------- 核心静态库 ----------------
add_library(udp_core STATIC
    src/config.cpp
    src/logger.cpp
    src/parser.cpp
    src/app.cpp
)
target_include_directories(udp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# 静态库的链接依赖要用 PUBLIC：udp_core 是静态库，它的 .o 里引用了 glog 的符号，
# 所以任何链接 udp_core 的目标都必须同时链接 glog，否则会报
# "undefined reference to google::...". 用 PRIVATE 在静态库上是最常见的踩坑点。
target_link_libraries(udp_core
    PUBLIC  udp_options
    PUBLIC  glog::glog
    PUBLIC  Threads::Threads
    PRIVATE yaml-cpp::yaml-cpp
)

# ---------------- 主程序 ----------------
# glog / Threads 会经由 udp_core 的 PUBLIC 依赖自动传递过来，无需重复声明。
add_executable(udp_logger src/main.cpp)
target_link_libraries(udp_logger
    PRIVATE udp_core
)

# ---------------- 测试工具：MCU 模拟器 ----------------
# 它不需要 glog / yaml-cpp，只用 socket + C++ 标准库。
add_executable(udp_sender tools/udp_sender.cpp)
target_link_libraries(udp_sender PRIVATE udp_options PRIVATE Threads::Threads)

# ---------------- 便利目标 ----------------
# 说明：make run 需要 config/config.yaml 的路径正确，所以这里要求
# 在工程根目录执行（或者用 scripts/run.sh）。

enable_testing()

add_test(NAME help_output COMMAND udp_logger --help)
add_test(NAME sender_help   COMMAND udp_sender --help)

message(STATUS "")
message(STATUS "构建完成后可执行文件位于: ${CMAKE_BINARY_DIR}")
message(STATUS "  主程序:     ${CMAKE_BINARY_DIR}/udp_logger")
message(STATUS "  MCU 模拟器: ${CMAKE_BINARY_DIR}/udp_sender")
message(STATUS "运行方式: 在工程根目录执行 ./build/udp_logger -f config/config.yaml")
message(STATUS "")
