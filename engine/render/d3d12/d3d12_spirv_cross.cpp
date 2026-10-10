// d3d12_spirv_cross.cpp — SPIR-V → HLSL，基于 spirv-cross-c-shared.dll 运行时加载。
//
// 设计要点（与 vk_glsl_compiler.cpp 的 shaderc 加载策略保持一致）：
//   * LoadLibrary/dlopen 运行时加载，不链 import lib → GryceRenderer.dll 对
//     SPIRV-Cross 零依赖；GL/Vulkan-only 运行时无需该 DLL。
//   * 加载失败 → spirv_to_hlsl 返回 false，调用方（D3D12Shader）降级；
//     D3D12 后端本就在没有着色器链时返回空句柄，不影响 GL/VK 主路径。
//   * C API 的不透明句柄在本文件用 void* 表示，枚举值按官方 spirv_cross_c.h
//     抄录（不能猜：option 值带 backend 位段，写错会被驱动拒绝）。

#include "d3d12_spirv_cross.h"

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

namespace gryce_engine::render::d3d12 {

namespace {

// ---- spirv_cross_c.h 的 ABI 子集（本地声明，不引第三方头）----
using spvc_context = void*;
using spvc_parsed_ir = void*;
using spvc_compiler = void*;
using spvc_compiler_options = void*;
// spvc_result: SPVC_SUCCESS = 0，其余为错误。
using spvc_result = int;
// spvc_bool: 0 / 1。
using spvc_bool = unsigned;

constexpr int kSpvcSuccess = 0;

// spvc_backend
constexpr unsigned kSpvcBackendHlsl = 2;
// spvc_capture_mode
constexpr unsigned kSpvcCaptureModeTakeOwnership = 1;

// spvc_compiler_option（含 backend 位段，务必照抄）
constexpr unsigned kOptionCommonBit = 0x1000000u;
constexpr unsigned kOptionHlslBit   = 0x4000000u;
constexpr unsigned kOptionFixupDepthConvention     = 3u | kOptionCommonBit;
constexpr unsigned kOptionFlipVertexY              = 4u | kOptionCommonBit;
constexpr unsigned kOptionHlslShaderModel          = 13u | kOptionHlslBit;
constexpr unsigned kOptionHlslUseEntryPointName    = 90u | kOptionHlslBit;
// SPVC_COMPILER_OPTION_HLSL_SUPPORT_NONZERO_BASE_VERTEX_BASE_INSTANCE
constexpr unsigned kOptionHlslNonZeroBaseVertex    = 16u | kOptionHlslBit;
// SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT / POINT_COORD_COMPAT（2D/粒子可能用到）
constexpr unsigned kOptionHlslPointSizeCompat      = 14u | kOptionHlslBit;
constexpr unsigned kOptionHlslPointCoordCompat     = 15u | kOptionHlslBit;
// SPVC_COMPILER_OPTION_HLSL_FLATTEN_MATRIX_VERTEX_INPUT_SEMANTICS
// 顶点输入是矩阵/数组属性时必须打开，否则 SPIRV-Cross 生成的 struct 成员会
// 带矩阵维数语义（DXGI 不认）。本项目 MeshVertex 无矩阵属性，但打开无害。
constexpr unsigned kOptionHlslFlattenMatrixVertexInput = 71u | kOptionHlslBit;

// ---- 动态加载 ----
//
// spvc_hlsl_vertex_attribute_remap：把顶点属性 location 映射到指定 HLSL 语义名。
// ★ 必须用这个 —— SPIRV-Cross 默认按 SPIR-V **变量名** 生成语义
// （`aPos` → TEXCOORD0），而 D3D12 输入布局是**按语义名匹配**的，
// 于是 POSITION 永远配不上、D3D12 输入全零 → 画面全黑。
struct HlslVertexAttributeRemap {
    unsigned location;
    const char* semantic;
};

using ContextCreateFn = spvc_result (*)(spvc_context*);
using ContextDestroyFn = void (*)(spvc_context);
using ContextGetLastErrorFn = const char* (*)(spvc_context);
using ContextParseSpirvFn = spvc_result (*)(spvc_context, const uint32_t*, size_t, spvc_parsed_ir*);
using ContextCreateCompilerFn = spvc_result (*)(spvc_context, unsigned, spvc_parsed_ir, unsigned,
                                                spvc_compiler*);
using CompilerCreateOptionsFn = spvc_result (*)(spvc_compiler, spvc_compiler_options*);
using CompilerOptionsSetBoolFn = spvc_result (*)(spvc_compiler_options, unsigned, spvc_bool);
using CompilerOptionsSetUintFn = spvc_result (*)(spvc_compiler_options, unsigned, unsigned);
using CompilerInstallOptionsFn = spvc_result (*)(spvc_compiler, spvc_compiler_options);
using CompilerCompileFn = spvc_result (*)(spvc_compiler, const char**);
using CompilerAddVertexAttributeRemapFn = spvc_result (*)(spvc_compiler,
                                                         const HlslVertexAttributeRemap*, size_t);

#ifdef _WIN32
using LibHandle = HMODULE;
static LibHandle load_library(const char* name) { return LoadLibraryA(name); }
static void* load_symbol(LibHandle lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
}
static void unload_library(LibHandle lib) { if (lib) FreeLibrary(lib); }
#else
using LibHandle = void*;
static LibHandle load_library(const char* name) { return dlopen(name, RTLD_LAZY | RTLD_LOCAL); }
static void* load_symbol(LibHandle lib, const char* name) { return dlsym(lib, name); }
static void unload_library(LibHandle lib) { if (lib) dlclose(lib); }
#endif

struct SpvcApi {
    ContextCreateFn         context_create = nullptr;
    ContextDestroyFn        context_destroy = nullptr;
    ContextGetLastErrorFn   get_last_error = nullptr;
    ContextParseSpirvFn     parse_spirv = nullptr;
    ContextCreateCompilerFn create_compiler = nullptr;
    CompilerCreateOptionsFn create_options = nullptr;
    CompilerOptionsSetBoolFn set_bool = nullptr;
    CompilerOptionsSetUintFn set_uint = nullptr;
    CompilerInstallOptionsFn install_options = nullptr;
    CompilerCompileFn       compile = nullptr;
    CompilerAddVertexAttributeRemapFn add_vertex_attribute_remap = nullptr;
    bool valid = false;
};

std::once_flag g_load_flag;
SpvcApi g_api;
bool g_attempted = false;

template <typename T> void set_sym(T& slot, LibHandle lib, const char* name) {
    slot = reinterpret_cast<T>(load_symbol(lib, name));
}

void do_load() {
    g_attempted = true;

    // 候选搜索：%VULKAN_SDK%/Bin 优先，再按默认搜索（exe 目录 / System32 / cwd / PATH）。
#ifdef _WIN32
    LibHandle lib = nullptr;
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk && *sdk) {
        std::string p = std::string(sdk) + "\\Bin\\spirv-cross-c-shared.dll";
        lib = LoadLibraryA(p.c_str());
    }
    if (!lib) lib = load_library("spirv-cross-c-shared.dll");
#else
    LibHandle lib = nullptr;
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk && *sdk) {
        lib = load_library((std::string(sdk) + "/lib/libspirv-cross-c-shared.so").c_str());
        if (!lib) lib = load_library((std::string(sdk) + "/bin/libspirv-cross-c-shared.so").c_str());
    }
    if (!lib) lib = load_library("libspirv-cross-c-shared.so");
#endif

    if (!lib) {
        GLOG_WARN("SPIRV-Cross: 'spirv-cross-c-shared' not found; "
                  "D3D12 shader translation unavailable (需要 Vulkan SDK 或把该 DLL 放到 exe 旁)");
        return;
    }

    set_sym(g_api.context_create,    lib, "spvc_context_create");
    set_sym(g_api.context_destroy,   lib, "spvc_context_destroy");
    set_sym(g_api.get_last_error,    lib, "spvc_context_get_last_error_string");
    set_sym(g_api.parse_spirv,       lib, "spvc_context_parse_spirv");
    set_sym(g_api.create_compiler,   lib, "spvc_context_create_compiler");
    set_sym(g_api.create_options,    lib, "spvc_compiler_create_compiler_options");
    set_sym(g_api.set_bool,          lib, "spvc_compiler_options_set_bool");
    set_sym(g_api.set_uint,          lib, "spvc_compiler_options_set_uint");
    set_sym(g_api.install_options,   lib, "spvc_compiler_install_compiler_options");
    set_sym(g_api.compile,           lib, "spvc_compiler_compile");
    set_sym(g_api.add_vertex_attribute_remap, lib,
            "spvc_compiler_hlsl_add_vertex_attribute_remap");

    g_api.valid = g_api.context_create && g_api.context_destroy && g_api.parse_spirv &&
                  g_api.create_compiler && g_api.create_options && g_api.set_bool &&
                  g_api.set_uint && g_api.install_options && g_api.compile;
    if (g_api.valid) {
        GLOG_INFO("SPIRV-Cross: loaded (runtime), SPIR-V -> HLSL translation enabled");
    } else {
        GLOG_WARN("SPIRV-Cross: loaded but required symbols missing; translation disabled");
    }
}

const SpvcApi& api() {
    std::call_once(g_load_flag, do_load);
    return g_api;
}

} // namespace

bool spirv_cross_available() {
    const SpvcApi& a = api();
    (void)g_attempted;
    return a.valid;
}

bool spirv_to_hlsl(const std::vector<uint32_t>& spirv, SpvcStage stage, const std::string& entry,
                   const std::vector<VertexSemantic>& vertex_semantics,
                   std::string& out_hlsl, std::string& error) {
    out_hlsl.clear();
    error.clear();

    const SpvcApi& a = api();
    if (!a.valid) {
        error = "spirv-cross not available";
        return false;
    }
    if (spirv.empty()) {
        error = "empty SPIR-V input";
        return false;
    }

    // 入口点名：SPIR-V 里统一是 "main"（引擎 GLSL 都是 void main()）。
    const std::string entry_name = entry.empty() ? std::string("main") : entry;

    spvc_context ctx = nullptr;
    if (a.context_create(&ctx) != kSpvcSuccess || !ctx) {
        error = "spvc_context_create failed";
        return false;
    }

    // 作用域退出时自动销毁 context（内部所有子对象随之一并释放）。
    struct CtxGuard {
        spvc_context c;
        ContextDestroyFn destroy;
        ~CtxGuard() { if (c && destroy) destroy(c); }
    } guard{ctx, a.context_destroy};

    spvc_parsed_ir ir = nullptr;
    spvc_result r = a.parse_spirv(ctx, spirv.data(), spirv.size(), &ir);
    if (r != kSpvcSuccess || !ir) {
        const char* e = a.get_last_error ? a.get_last_error(ctx) : nullptr;
        error = e ? e : "spvc_context_parse_spirv failed";
        return false;
    }

    spvc_compiler compiler = nullptr;
    r = a.create_compiler(ctx, kSpvcBackendHlsl, ir, kSpvcCaptureModeTakeOwnership, &compiler);
    if (r != kSpvcSuccess || !compiler) {
        const char* e = a.get_last_error ? a.get_last_error(ctx) : nullptr;
        error = e ? e : "spvc_context_create_compiler(HLSL) failed";
        return false;
    }

    spvc_compiler_options opts = nullptr;
    r = a.create_options(compiler, &opts);
    if (r != kSpvcSuccess || !opts) {
        error = "spvc_compiler_create_compiler_options failed";
        return false;
    }

    // ---- 选项 ----
    // 1) HLSL shader model 5.0（DXC 接受；SM 5.0 兼容面最广）。
    a.set_uint(opts, kOptionHlslShaderModel, 50u);
    // 2) 用入口点名（引擎统一 "main"，生成 HLSL 的入口也叫 main）。
    a.set_bool(opts, kOptionHlslUseEntryPointName, 1u);
    // 3) **关闭** "支持非零 baseVertex/baseInstance"。
    //    打开它（=1）时 SPIRV-Cross 会为用到 gl_VertexIndex / gl_InstanceIndex 的
    //    着色器额外发出一个
    //        cbuffer SPIRV_Cross_VertexInfo { int BaseVertex; int BaseInstance; };
    //    （隐式落在 register(b1)）。本项目的图形根签名只覆盖 b0（VS/PS 各一个），
    //    于是 `CreateGraphicsPipelineState` 直接失败：
    //        "Root Signature doesn't match Vertex Shader: Shader CBV descriptor range
    //         (BaseShaderRegister=1, ...) is not fully bound in root signature"
    //    —— 表现为 instance 化着色器（instanced.vert 用 gl_InstanceID 取 SSBO）
    //    永远建不出 PSO。
    //    关掉之后 gl_InstanceIndex 直接映射成 SV_InstanceID，这正是本引擎的语义：
    //    所有绘制（含 ExecuteIndirect）的 baseVertex / baseInstance 恒为 0
    //    （cull_instances.comp 写间接参数时 first=0、baseInstance=0）。
    a.set_bool(opts, kOptionHlslNonZeroBaseVertex, 0u);
    // 4) 点精灵兼容（2D / 粒子 pass 可能用到 gl_PointSize / gl_PointCoord）。
    a.set_bool(opts, kOptionHlslPointSizeCompat, 1u);
    a.set_bool(opts, kOptionHlslPointCoordCompat, 1u);

    // ★ 坐标约定（对应计划第 5 节的平台差异）：
    //   D3D 与 GL 的裁剪空间 Z 范围不同（GL [-1,1] / D3D [0,1]），且 Y 方向相反。
    //   SPIRV-Cross 能替我们在翻译期修正：
    //     - FIXUP_DEPTH_CONVENTION：把 GL 风格的 [0,w] 深度重映射到 D3D 的 [0,w]
    //       （GL 的 gl_Position.z 是 [-w,w]，需要 remap 到 [0,w]）。
    //     - FLIP_VERTEX_Y：翻转顶点 Y（D3D 的 NDC Y 向下）。
    //   这两项让引擎侧**不需要**改 Matrix4f::perspective，从而不触碰 math_camera 的
    //   CTest 断言（计划里列为中风险项，这里用翻译期修正规避）。
    a.set_bool(opts, kOptionFixupDepthConvention, 1u);
    if (stage == SpvcStage::Vertex) {
        a.set_bool(opts, kOptionFlipVertexY, 1u);
    }

    r = a.install_options(compiler, opts);
    if (r != kSpvcSuccess) {
        const char* e = a.get_last_error ? a.get_last_error(ctx) : nullptr;
        error = e ? e : "spvc_compiler_install_compiler_options failed";
        return false;
    }

    // ★ 顶点属性语义重映射：把 location N 改成 POSITION/NORMAL/... 语义名。
    // D3D12 输入布局按语义名匹配，不做这一步会全部落空（顶点数据读成 0）。
    if (stage == SpvcStage::Vertex && !vertex_semantics.empty() && a.add_vertex_attribute_remap) {
        std::vector<HlslVertexAttributeRemap> remaps;
        remaps.reserve(vertex_semantics.size());
        for (const VertexSemantic& vs : vertex_semantics) {
            remaps.push_back(HlslVertexAttributeRemap{vs.location, vs.semantic.c_str()});
        }
        r = a.add_vertex_attribute_remap(compiler, remaps.data(), remaps.size());
        if (r != kSpvcSuccess) {
            const char* e = a.get_last_error ? a.get_last_error(ctx) : nullptr;
            // 非致命：语义没重映射成功仍然产出 HLSL，只是输入布局会配不上；
            // 记进 error 便于上层告警，但不中止。
            GLOG_WARN("SPIRV-Cross: add_vertex_attribute_remap failed: {}",
                      e ? e : "(no diagnostics)");
        }
    }

    const char* source = nullptr;
    r = a.compile(compiler, &source);
    if (r != kSpvcSuccess || !source) {
        const char* e = a.get_last_error ? a.get_last_error(ctx) : nullptr;
        error = e ? e : "spvc_compiler_compile failed";
        return false;
    }

    out_hlsl = source;
    return !out_hlsl.empty();
}

} // namespace gryce_engine::render::d3d12
