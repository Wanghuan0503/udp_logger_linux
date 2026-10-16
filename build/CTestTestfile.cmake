# CMake generated Testfile for 
# Source directory: /home/wh/桌面/udp_logger
# Build directory: /home/wh/桌面/udp_logger/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(help_output "/home/wh/桌面/udp_logger/build/udp_logger" "--help")
set_tests_properties(help_output PROPERTIES  _BACKTRACE_TRIPLES "/home/wh/桌面/udp_logger/CMakeLists.txt;87;add_test;/home/wh/桌面/udp_logger/CMakeLists.txt;0;")
add_test(sender_help "/home/wh/桌面/udp_logger/build/udp_sender" "--help")
set_tests_properties(sender_help PROPERTIES  _BACKTRACE_TRIPLES "/home/wh/桌面/udp_logger/CMakeLists.txt;88;add_test;/home/wh/桌面/udp_logger/CMakeLists.txt;0;")
