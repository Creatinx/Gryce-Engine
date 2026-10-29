#pragma once

// ---------------------------------------------------------------------------
// super_resolution — 超分与 NVIDIA DLSS 接入层
//
// 职责：把「画面重建用哪种超分」这件事从具体后端里抽出来，并提供 DLSS 运行时
// 的探测结果（可编可测，不依赖任何 DLSS 二进制被真正安装）。
//
// 三种模式：
//   Off      原生分辨率，不做重建（后端直出，行为与未引入本模块时一致）。
//   Temporal 引擎内置时域重建：亚像素抖动 + 重投影累积 + 深度引导双边上采样。
//            当前作用于硬件光追反射（见 vk_ray_query_pass 的 trace/temporal/
//            composite 三段），不需要任何第三方运行时。
//   Dlss     NVIDIA DLSS。需要三件运行时同时到位：Streamline 的 sl.interposer.dll
//            与 sl.dlss.dll、NGX 的 nvngx_dlss.dll（随 DLSS SDK / 游戏附带分发）。
//            头文件已在 third_party/streamline/include（SL 2.14.16），运行时不在
//            仓库内 —— 因此本模块在缺运行时的情况下如实回报不可用，绝不假装成功。
//
// 探测只做两件事：文件存在性 + 关键导出符号可解析性。它不改变设备创建路径，
// 因此对现有渲染流程零副作用（缺失即降级，日志说明缺什么、去哪拿）。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace gryce_engine::render {

enum class SuperResolutionMode {
    Off = 0,
    Temporal = 1,
    Dlss = 2,
};

struct SuperResolutionCaps {
    SuperResolutionMode mode = SuperResolutionMode::Off;
    // sl.interposer.dll 可加载且 slInit / slSetFeatureLoaded 可解析
    bool streamline_runtime = false;
    // nvngx_dlss.dll 存在（NGX 的 DLSS 实现本体）
    bool ngx_runtime = false;
    // 设备创建是否已改走 interposer 代理（DLSS 的硬前置，当前未接线）
    bool device_proxied = false;
    // 以上全部成立才算「可用」
    bool dlss_usable = false;
    std::string interposer_path;   // 找到的 interposer 绝对路径（空 = 没找到）
    std::string detail;            // 一行人类可读结论（日志/UI 直接展示）
};

// 探测 DLSS 运行时。结果进程内缓存（只探一次）；extra_dirs 用于追加搜索目录
// （项目目录、编辑器插件目录等），优先级低于可执行文件所在目录。
const SuperResolutionCaps& probe_super_resolution(const std::vector<std::string>& extra_dirs = {});

// 模式名（"off" / "temporal" / "dlss"），用于日志与配置回显。
const char* super_resolution_mode_name(SuperResolutionMode mode);

// 从配置字符串解析模式（大小写不敏感；未知值回落到 Temporal）。
SuperResolutionMode parse_super_resolution_mode(const std::string& text);

} // namespace gryce_engine::render
