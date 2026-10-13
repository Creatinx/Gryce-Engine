// vk_glsl_compiler.cpp — GLSL → SPIR-V 运行时编译，基于 shaderc 动态加载。
//
// 设计要点：
//   - 通过 LoadLibrary/dlopen 在运行时加载 shaderc_shared.dll / .so，而非链接期
//     import lib。这样 GryceRenderer.dll 对 shaderc 零依赖：
//       * GL-only 游戏运行时无需任何 shaderc DLL；
//       * 免去 MinGW 链接 MSVC 静态库（.drectve 不兼容）的坑（现场已验证：
//         214MB 的 shaderc_combined.lib 无法被 g++ 链接）；
//       * shaderc_shared.dll 完全自包含（仅依赖 MSVC 运行库 + KERNEL32）。
//   - 加载失败 / 不可用时返回 false，由调用方降级到 `.spv` 预编译产物，
//     "首次运行才编译"是渐进增强而非硬依赖。

#include "vk_glsl_compiler.h"

#include "utils/glog/glog_lib.h"

#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <cstdlib>
#endif

namespace gryce_engine::render {

namespace {

// shaderc C API 的子集声明（与 Vulkan SDK <shaderc/shaderc.h> + status.h 一致）。
using CompilerInitFn = void* (*)();
using CompilerReleaseFn = void (*)(void* compiler);
using CompileIntoSpvFn = void* (*)(void* compiler, const char* source, size_t size,
                                   int shader_kind, const char* input_name,
                                   const char* entry_name, void* options);
using ResultGetStatusFn = int (*)(const void* result);
using ResultGetErrorFn = const char* (*)(const void* result);
using ResultGetLengthFn = size_t (*)(const void* result);
using ResultGetBytesFn = const char* (*)(const void* result);
using ResultReleaseFn = void (*)(const void* result);
// 计算管线靠 SPIR-V 里的 OpName 拿 SSBO 块名 → 必须让 glslang 吐出调试名。
using CompileOptionsNewFn = void* (*)();
using CompileOptionsFreeFn = void (*)(void* options);
using CompileOptionsSetDebugInfoFn = void (*)(void* options, bool generate);
// 目标环境：SSBO 在 Vulkan 语义下才是 StorageBuffer 存储类（GL 语义是
// Uniform + BufferBlock，在 Vulkan 上不能当 SSBO 用）。
using CompileOptionsSetTargetEnvFn = void (*)(void* options, int target, uint32_t version);

// shaderc_vertex_shader / shaderc_fragment_shader / shaderc_compute_shader
// 的实际枚举值（shaderc_shader_kind 首个成员为 0）。
constexpr int kShadercVertexShader = 0;
constexpr int kShadercFragmentShader = 1;
constexpr int kShadercComputeShader = 2;
// 编译成功状态（shaderc_compilation_status_success = 0）。
constexpr int kCompilationSuccess = 0;
// shaderc_target_env_vulkan = 0。
constexpr int kShadercTargetEnvVulkan = 0;
// 目标版本必须是 **vulkan 1.2**（不能是 1.0）：SSBO 在 SPIR-V 里的表示随目标
// 版本变化——1.0 目标产出 SPIR-V 1.0，其中 SSBO 是旧式表示（storage class
// Uniform + BufferBlock 装饰，即 UBO 语义）；从 1.1 目标（SPIR-V 1.3）起才改为
// 现代表示（storage class StorageBuffer + Block 装饰）。
// 引擎给 compute 块建的是 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER 描述符
// （vk_compute.cpp），旧式表示会让着色器侧变成只读 UBO 语义，
// DstModels / DrawArgsBuffer 的写入被静默丢弃（instanceCount 恒为 0，
// 间接绘制一个实例都不出）。本机实测 glslangValidator：
//   --target-env vulkan1.0 → Block=0, StorageBuffer vars=0
//   --target-env vulkan1.1 → Block=1, StorageBuffer vars=1
// 引擎 app_info.apiVersion = VK_API_VERSION_1_2，故 1.2 目标安全。
// 版本字编码为 (major << 16) | (minor << 8)：vulkan 1.2 = 0x00010200。
// 注意不能写成 (2 << 16)——那是非法版本号，shaderc 会静默回退到默认（1.0），
// 于是又拿到旧式 SSBO 表示，问题看似"改了没用"。
constexpr uint32_t kShadercTargetEnvVersionVulkan12 = (1u << 16) | (2u << 8);

#ifdef _WIN32
using LibHandle = HMODULE;
using SymFn = void (*)();
static LibHandle load_library(const char* name) { return LoadLibraryA(name); }
static void* load_symbol(LibHandle lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
}
#else
using LibHandle = void*;
static LibHandle load_library(const char* name) { return dlopen(name, RTLD_LAZY | RTLD_LOCAL); }
static void* load_symbol(LibHandle lib, const char* name) { return dlsym(lib, name); }
#endif

struct ShadercApi {
    CompilerInitFn init = nullptr;
    CompilerReleaseFn release = nullptr;
    CompileIntoSpvFn compile_into_spv = nullptr;
    ResultGetStatusFn get_status = nullptr;
    ResultGetErrorFn get_error = nullptr;
    ResultGetLengthFn get_length = nullptr;
    ResultGetBytesFn get_bytes = nullptr;
    ResultReleaseFn result_release = nullptr;
    CompileOptionsNewFn options_new = nullptr;
    CompileOptionsFreeFn options_free = nullptr;
    CompileOptionsSetDebugInfoFn options_set_debug_info = nullptr;
    // 可选符号：缺失时静默跳过（老版本 shaderc 也能用，只是拿不到 Vulkan 语义）。
    CompileOptionsSetTargetEnvFn options_set_target_env = nullptr;
    bool valid = false;
};

ShadercApi& g_api() {
    static ShadercApi api;
    return api;
}
std::once_flag g_load_flag;

template <typename T> void set_sym(T& slot, LibHandle lib, const char* name) {
    slot = reinterpret_cast<T>(load_symbol(lib, name));
}

void do_load() {
    // Windows 先尝试带路径候选（%VULKAN_SDK%/Bin），提高定位成功率；失败再按
    // LoadLibraryA 默认搜索（exe 目录 / System32 / cwd / PATH）。POSIX 同理。
#ifdef _WIN32
    std::string sdk = std::getenv("VULKAN_SDK") ? std::getenv("VULKAN_SDK") : "";
    std::string candidate = sdk.empty() ? "" : (sdk + "\\Bin\\shaderc_shared.dll");
    LibHandle lib = candidate.empty() ? nullptr : LoadLibraryA(candidate.c_str());
    if (!lib) lib = load_library("shaderc_shared.dll");
#else
    LibHandle lib = nullptr;
    const char* vk = std::getenv("VULKAN_SDK");
    if (vk) {
        lib = load_library((std::string(vk) + "/bin/libshaderc_shared.so").c_str());
    }
    if (!lib) lib = load_library("libshaderc_shared.so");
    if (!lib) lib = load_library("libshaderc_combined.so");
#endif

    if (!lib) {
        GLOG_WARN("shaderc: not found, Vulkan shaders fall back to pre-compiled SPIR-V");
        return;
    }

    ShadercApi& a = g_api();
    set_sym(a.init, lib, "shaderc_compiler_initialize");
    set_sym(a.release, lib, "shaderc_compiler_release");
    set_sym(a.compile_into_spv, lib, "shaderc_compile_into_spv");
    set_sym(a.get_status, lib, "shaderc_result_get_compilation_status");
    set_sym(a.get_error, lib, "shaderc_result_get_error_message");
    set_sym(a.get_length, lib, "shaderc_result_get_length");
    set_sym(a.get_bytes, lib, "shaderc_result_get_bytes");
    set_sym(a.result_release, lib, "shaderc_result_release");
    // 注意 new/free 与 initialize/release 是同一对函数的两代命名（签名完全一致：
    // 无参构造 / 析构）。部分 shaderc 构建（实测 VulkanSDK 1.4.350.0 的
    // shaderc_shared.dll）只导出老版 initialize/release，若只按新名加载，
    // options_new 会是 nullptr → options 块被整体跳过 → target_env 与 debug info
    // 全都静默不生效（compute 按 GL 语义产出 Uniform+BufferBlock，SSBO 绑不上）。
    // 因此两代名字都要试，任一命中即可。
    set_sym(a.options_new, lib, "shaderc_compile_options_new");
    if (!a.options_new) set_sym(a.options_new, lib, "shaderc_compile_options_initialize");
    set_sym(a.options_free, lib, "shaderc_compile_options_free");
    if (!a.options_free) set_sym(a.options_free, lib, "shaderc_compile_options_release");
    set_sym(a.options_set_debug_info, lib, "shaderc_compile_options_set_generate_debug_info");
    set_sym(a.options_set_target_env, lib, "shaderc_compile_options_set_target_env");

    if (!a.options_new || !a.options_free) {
        GLOG_WARN("shaderc: compile options API missing "
                  "(shaderc_compile_options_new/initialize), "
                  "Vulkan compute shaders will be compiled without target env "
                  "(SSBO semantics may be wrong)");
    }

    a.valid = a.init && a.release && a.compile_into_spv && a.get_status &&
              a.get_error && a.get_length && a.get_bytes && a.result_release;
    // 让库句柄常驻进程生命周期：保持全局引用，不卸载，避免析构顺序问题。
    static LibHandle s_kept_handle = lib;
    (void)s_kept_handle;

    if (!a.valid) {
        GLOG_WARN("shaderc: library loaded but required symbols missing, "
                  "Vulkan shaders fall back to pre-compiled SPIR-V");
    } else {
        GLOG_INFO("shaderc: loaded 'shaderc_shared', Vulkan shaders compiled at first run");
    }
}

void ensure_loaded() {
    std::call_once(g_load_flag, do_load);
}

// 常驻 compiler 实例：shaderc_compiler_initialize 会初始化 glslang 的全局状态，
// 单次成本在数十毫秒量级。若每个 stage 都新建/释放一次，启动耗时就会随 shader
// 数量线性增长（本项目 3D + 2D 合计 40+ 个 stage）。这里只创建一次并常驻到进程
// 结束——与 shaderc 库句柄同样不卸载，规避析构顺序问题。
// 注意：shaderc 的 compiler 实例不是线程安全的，故用互斥锁把整段编译串行化。
struct ResidentCompiler {
    void* handle = nullptr;
    bool created = false;
    std::mutex mutex;
};

ResidentCompiler& g_compiler() {
    static ResidentCompiler c;
    return c;
}

// 返回已加锁的 unique_lock（调用方持有到编译结束即自动解锁），并输出常驻实例。
std::unique_lock<std::mutex> acquire_compiler(void*& compiler) {
    ResidentCompiler& c = g_compiler();
    std::unique_lock<std::mutex> lock(c.mutex);
    if (!c.created) {
        ShadercApi& a = g_api();
        c.handle = (a.valid && a.init) ? a.init() : nullptr;
        c.created = true;
        if (!c.handle) {
            GLOG_WARN("shaderc: shaderc_compiler_initialize failed, "
                      "Vulkan shaders fall back to pre-compiled SPIR-V");
        }
    }
    compiler = c.handle;
    return lock;
}

} // namespace

bool shaderc_available() {
    ensure_loaded();
    return g_api().valid;
}

bool compile_glsl_to_spirv(const std::string& source, const std::string& file_name,
                           GlslStage stage, std::vector<uint32_t>& out, std::string& error) {
    ensure_loaded();
    ShadercApi& a = g_api();
    if (!a.valid || !a.init || !a.compile_into_spv) {
        error = "shaderc unavailable";
        return false;
    }

    // 复用常驻实例（插件持有互斥锁直到本次编译结束），避免每个 stage 重新初始化
    // glslang 全局状态。
    void* compiler = nullptr;
    std::unique_lock<std::mutex> lock = acquire_compiler(compiler);
    if (!compiler) {
        error = "shaderc_compiler_initialize failed";
        return false;
    }

    const int kind = stage == GlslStage::Vertex    ? kShadercVertexShader
                   : stage == GlslStage::Fragment ? kShadercFragmentShader
                                                  : kShadercComputeShader;
    const char* name = file_name.empty() ? "shader" : file_name.c_str();

    // 计算阶段显式生成调试信息（安全网）：VulkanComputePipeline 靠 SPIR-V 的
    // OpName 取得 SSBO 块名。实测 glslang 默认已保留**结构体类型名**（块名就在
    // 类型上），但局部变量名会被剥掉；显式打开可保证块名一定在，且计算着色器
    // 数量极少（1~2 个），SPIR-V 体积差异可忽略。图形管线不需要块名，保持原样
    // 以免影响既有 .spv 缓存。
    void* options = nullptr;
    if (kind == kShadercComputeShader && a.options_new && a.options_free) {
        options = a.options_new();
        if (options) {
            if (a.options_set_debug_info) a.options_set_debug_info(options, true);
            // 计算着色器必须按 **Vulkan 语义 + 现代 SSBO 表示**编译：
            //   - 源码用 `#version 450`（无 core profile）书写；
            //   - 目标环境显式设为 Vulkan 1.2（不能用 1.0，理由见上方常量注释）。
            // 旧式表示（Uniform + BufferBlock）在 Vulkan 上是只读 UBO 语义，
            // writeonly 的 DstModels 与读写的 DrawArgsBuffer 写入会被静默丢弃，
            // 间接绘制的 instanceCount 恒为 0（实例全灭）。
            // 图形着色器仍走默认选项（不受影响）。
            if (a.options_set_target_env) {
                a.options_set_target_env(options, kShadercTargetEnvVulkan,
                                         kShadercTargetEnvVersionVulkan12);
            }
        }
    }

    void* result = a.compile_into_spv(compiler, source.data(), source.size(), kind,
                                      name, "main", options);
    if (options && a.options_free) a.options_free(options);
    if (!result) {
        error = "shaderc_compile_into_spv returned null";
        return false;
    }

    const int status = a.get_status ? a.get_status(result) : -1;
    const char* err_msg = a.get_error ? a.get_error(result) : nullptr;
    const size_t len = a.get_length ? a.get_length(result) : 0;
    const char* bytes = a.get_bytes ? a.get_bytes(result) : nullptr;

    if (status != kCompilationSuccess || !bytes || len == 0 || len % 4 != 0) {
        std::string diag = err_msg ? err_msg : "(no diagnostics)";
        a.result_release(result);
        error = diag;
        return false;
    }

    out.assign(reinterpret_cast<const uint32_t*>(bytes),
               reinterpret_cast<const uint32_t*>(bytes) + len / 4);

    a.result_release(result);
    return true;
}

} // namespace gryce_engine::render