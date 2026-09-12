// 平台/路径工具：当前 exe 目录、运行时 DLL 定位、shader 目录、MSVC CRT 目录。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gryce_engine::gc {

// 将控制台输出切换为 UTF-8（Windows 默认 GBK 会乱码 中文输出）。
void init_utf8_console();

// 运行中的 GryceGC 可执行文件所在目录（正斜杠）。
std::string get_exe_dir();

// 在 dir 中按前缀查找引擎运行时 DLL，容忍 debug `d` 后缀与 `lib` 前缀。
// 如 "grycecore" 可匹配 GryceCore.dll / GryceCored.dll / libGryceCore.dll；
// "glfw3" 匹配 glfw3.dll / glfw3d.dll。无匹配返回空路径。
std::filesystem::path find_sibling_dll(const std::filesystem::path& dir,
                                       const std::string& prefix);

// 定位 Vulkan shader 编译器运行时 DLL（shaderc_shared.dll）。优先 VULKAN_SDK 环境变量，
// 回退默认安装根；GL-only 运行时不需要。
std::filesystem::path find_shaderc_dll();

// 依据 dir 中 core DLL 是否带 `d` 后缀，判断是否为 Debug 构建。
bool sibling_is_debug(const std::filesystem::path& dir);

// 定位 core 默认 shader 目录；优先随包部署副本，否则上溯仓库根 src/render/shaders。
// 以存在 forward_clustered 子目录作为有效标识，避免误命中。
std::filesystem::path find_core_shaders_dir(const std::filesystem::path& exe_dir);

// 运行时 DLL 候选位置：exe 同级、exe 的 runtime/、以及上溯若干层的
// GryceEngineUtils/lib 与 runtime/。
std::vector<std::filesystem::path> runtime_candidates(const std::filesystem::path& exe_dir);

// 返回第一个实际包含引擎 core DLL 的候选目录，无则返回空路径。
std::filesystem::path find_runtime_dir_near_exe(const std::filesystem::path& exe_dir);

// 定位包含目标 MSVC CRT 运行时 DLL 的目录（读 CMakeCache / 常见 VS 安装 / System32 兜底）。
std::filesystem::path find_msvc_crt_dir(const std::filesystem::path& build_dir, bool debug);

} // namespace gryce_engine::gc