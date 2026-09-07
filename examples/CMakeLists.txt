# examples/CMakeLists.txt —— Gryce Engine 示例项目
#
# 示例均为纯资源项目（GryceGC-A 标准）：不产生自包含宿主 exe，
# 玩法逻辑在 QuickJS（game.js）中实现，由 GryceGame 固定运行时
# （templates/GameTemplates.cpp）通过 GryceGC 打包并运行。
#
# 每个子目录是完整项目：scenes/ + scripts/ + resources + project.data
# （project.data 是唯一的配置文件，清单与运行时设置合并于一处）。
# 需要用 GryceGC 打包运行，而非作为 CMake 目标编译。