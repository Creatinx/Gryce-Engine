# CMake generated Testfile for 
# Source directory: D:/Gryce-Engine/toolchain/grycegc
# Build directory: D:/Gryce-Engine/toolchain/grycegc/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[grycegc_cli]=] "D:/Gryce-Engine/toolchain/grycegc/build/GryceGCTests.exe" "--suite" "cli")
set_tests_properties([=[grycegc_cli]=] PROPERTIES  LABELS "unit;grycegc" TIMEOUT "60" WORKING_DIRECTORY "D:/Gryce-Engine/toolchain/grycegc/build" _BACKTRACE_TRIPLES "D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;92;add_test;D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;0;")
add_test([=[grycegc_crc32]=] "D:/Gryce-Engine/toolchain/grycegc/build/GryceGCTests.exe" "--suite" "crc32")
set_tests_properties([=[grycegc_crc32]=] PROPERTIES  LABELS "unit;grycegc" TIMEOUT "60" WORKING_DIRECTORY "D:/Gryce-Engine/toolchain/grycegc/build" _BACKTRACE_TRIPLES "D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;92;add_test;D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;0;")
add_test([=[grycegc_pe]=] "D:/Gryce-Engine/toolchain/grycegc/build/GryceGCTests.exe" "--suite" "pe")
set_tests_properties([=[grycegc_pe]=] PROPERTIES  LABELS "unit;grycegc" TIMEOUT "60" WORKING_DIRECTORY "D:/Gryce-Engine/toolchain/grycegc/build" _BACKTRACE_TRIPLES "D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;92;add_test;D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;0;")
add_test([=[grycegc_aes]=] "D:/Gryce-Engine/toolchain/grycegc/build/GryceGCTests.exe" "--suite" "aes")
set_tests_properties([=[grycegc_aes]=] PROPERTIES  LABELS "unit;grycegc" TIMEOUT "60" WORKING_DIRECTORY "D:/Gryce-Engine/toolchain/grycegc/build" _BACKTRACE_TRIPLES "D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;92;add_test;D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;0;")
add_test([=[grycegc_smoke]=] "D:/Gryce-Engine/toolchain/grycegc/build/GryceGCTests.exe" "--suite" "smoke")
set_tests_properties([=[grycegc_smoke]=] PROPERTIES  LABELS "unit;grycegc" TIMEOUT "60" WORKING_DIRECTORY "D:/Gryce-Engine/toolchain/grycegc/build" _BACKTRACE_TRIPLES "D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;92;add_test;D:/Gryce-Engine/toolchain/grycegc/CMakeLists.txt;0;")
