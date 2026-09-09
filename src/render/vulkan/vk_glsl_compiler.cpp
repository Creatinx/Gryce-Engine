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

// shaderc_vertex_shader / shaderc_fragment_shader 的实际枚举值（首个成员为 0）。
constexpr int kShadercVertexShader = 0;
constexpr int kShadercFragmentShader = 1;
// 编译成功状态（shaderc_compilation_status_success = 0）。
constexpr int kCompilationSuccess = 0;

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

    void* compiler = a.init();
    if (!compiler) {
        error = "shaderc_compiler_initialize failed";
        return false;
    }

    const int kind = stage == GlslStage::Vertex ? kShadercVertexShader : kShadercFragmentShader;
    const char* name = file_name.empty() ? "shader" : file_name.c_str();
    void* result = a.compile_into_spv(compiler, source.data(), source.size(), kind,
                                      name, "main", nullptr);
    if (!result) {
        a.release(compiler);
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
        a.release(compiler);
        error = diag;
        return false;
    }

    out.assign(reinterpret_cast<const uint32_t*>(bytes),
               reinterpret_cast<const uint32_t*>(bytes) + len / 4);

    a.result_release(result);
    a.release(compiler);
    return true;
}

} // namespace gryce_engine::render