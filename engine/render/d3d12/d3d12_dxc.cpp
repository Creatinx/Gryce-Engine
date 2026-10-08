// d3d12_dxc.cpp — HLSL → DXIL，运行时加载 dxcompiler.dll。见头文件注释。

#include "d3d12_dxc.h"

#include "utils/glog/glog_lib.h"

#include <cstdio>
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

// ---------------------------------------------------------------------------
// 最小 COM 声明（只声明 ABI 需要的部分，不引 dxcapi.h）
//
// 为什么不 include dxcapi.h：它依赖 MSVC 的 objidl 系列（__declspec(uuid) /
// IUnknown 来自 <unknwn.h>），MinGW 头与之不兼容（本项目在 Windows SDK 头
// 上已确认过整片编译失败）。这里只按 ABI 布局声明，**vtable 顺序必须与
// dxcapi.h 完全一致**。
//
// ★ 命名注意：`HRESULT` / `REFIID` / `GUID` 在 MinGW 的 windows.h 里已存在，
// 不能再 `using` 同名别名（会报 "expected nested-name-specifier"）。
// 这里只定义**新名字**，并直接用系统的 HRESULT/GUID 类型。
// ---------------------------------------------------------------------------

using DxcHr = ::HRESULT;
using DxcGuid = ::GUID;

// 复用的系统类型（来自 windows.h）。
using DxcRefGuid = const ::GUID&;
using DxcRefIid  = const ::IID&;

struct IUnknownLocal {
    virtual DxcHr QueryInterface(DxcRefIid riid, void** ppv) = 0;
    virtual unsigned long AddRef() = 0;
    virtual unsigned long Release() = 0;
};

struct IDxcBlobLocal : IUnknownLocal {
    virtual void* GetBufferPointer() = 0;
    virtual size_t GetBufferSize() = 0;
};

struct IDxcOperationResultLocal : IUnknownLocal {
    virtual DxcHr GetStatus(DxcHr* pStatus) = 0;
    virtual DxcHr GetResult(IDxcBlobLocal** ppResult) = 0;
    virtual DxcHr GetErrorBuffer(void** ppErrors) = 0;
};

// ★ vtable 顺序严格照抄 dxcapi.h（IDxcResult 继承 IDxcOperationResult，
//   自身再追加 5 个虚函数）。
enum DXC_OUT_KIND_LOCAL {
    DXC_OUT_NONE_L = 0,
    DXC_OUT_OBJECT_L = 1,
    DXC_OUT_ERRORS_L = 2,
    DXC_OUT_PDB_L = 3,
    DXC_OUT_SHADER_HASH_L = 4,
    DXC_OUT_DISASSEMBLY_L = 5,
    DXC_OUT_HLSL_L = 6,
    DXC_OUT_TEXT_L = 7,
    DXC_OUT_REFLECTION_L = 8,
    DXC_OUT_ROOT_SIGNATURE_L = 9,
    DXC_OUT_EXTRA_OUTPUTS_L = 10,
    DXC_OUT_REMARKS_L = 11,
};

struct IDxcResultLocal : IDxcOperationResultLocal {
    virtual int HasOutput(int kind) = 0;
    virtual DxcHr GetOutput(int kind, DxcRefIid iid, void** ppvObject, void** ppOutputName) = 0;
    virtual unsigned GetNumOutputs() = 0;
    virtual int GetOutputByIndex(unsigned index) = 0;
    virtual int PrimaryOutput() = 0;
};

struct DxcBufferLocal {
    const void* ptr = nullptr;
    size_t size = 0;
    unsigned encoding = 0;   // DXC_CP_UTF8 = 65001
};

struct IDxcCompiler3Local : IUnknownLocal {
    virtual DxcHr Compile(const DxcBufferLocal* pSource, const wchar_t** pArguments,
                          unsigned argCount, void* pIncludeHandler, DxcRefIid riid,
                          void** ppResult) = 0;
};

// ---- GUID（手抄自 dxcapi.h / 官方头，绝不猜）----
const DxcGuid kClsidDxcCompiler = {
    0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
const DxcGuid kIidDxcCompiler3 = {
    0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
const DxcGuid kIidDxcResult = {
    0x58346cda, 0xdde7, 0x4497, {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};
const DxcGuid kIidDxcBlob = {
    0x8ba5fb08, 0x5195, 0x40e2, {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};

// ---- 动态加载 ----
using DxcCreateInstanceProc = DxcHr(__stdcall*)(DxcRefGuid rclsid, DxcRefIid riid, void** ppv);

#ifdef _WIN32
using LibHandle = HMODULE;
static LibHandle load_library(const char* name) { return LoadLibraryA(name); }
static void* load_symbol(LibHandle lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
}
#else
using LibHandle = void*;
static LibHandle load_library(const char* name) { return dlopen(name, RTLD_LAZY | RTLD_LOCAL); }
static void* load_symbol(LibHandle lib, const char* name) { return dlsym(lib, name); }
#endif

std::once_flag g_load_flag;
DxcCreateInstanceProc g_create_instance = nullptr;
bool g_attempted = false;

void do_load() {
    g_attempted = true;

    // 候选搜索：%VULKAN_SDK%/Bin 优先（本项目环境里 DXC 随 Vulkan SDK 一起装了），
    // 再按默认搜索（exe 目录 / System32 / cwd / PATH）。
    LibHandle lib = nullptr;
#ifdef _WIN32
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk && *sdk) {
        std::string p = std::string(sdk) + "\\Bin\\dxcompiler.dll";
        lib = LoadLibraryA(p.c_str());
    }
    if (!lib) lib = load_library("dxcompiler.dll");
#else
    const char* sdk = std::getenv("VULKAN_SDK");
    if (sdk && *sdk) {
        lib = load_library((std::string(sdk) + "/lib/libdxcompiler.so").c_str());
    }
    if (!lib) lib = load_library("libdxcompiler.so");
#endif

    if (!lib) {
        GLOG_WARN("DXC: 'dxcompiler' not found; D3D12 shader compilation unavailable "
                  "（需要 Vulkan SDK 的 dxcompiler.dll，或把它放到 exe 旁）");
        return;
    }

    g_create_instance = reinterpret_cast<DxcCreateInstanceProc>(
        load_symbol(lib, "DxcCreateInstance"));
    // dxcompiler 还依赖 dxil.dll 做校验（签名），但 **编译** 本身不强制需要它。
    // 这里不主动加载 dxil.dll：DXC 内部会按需找；找不到时仅校验步骤失败。

    static LibHandle s_kept_handle = lib;   // 常驻，不卸载（避免析构顺序问题）
    (void)s_kept_handle;

    if (!g_create_instance) {
        GLOG_WARN("DXC: 'dxcompiler' loaded but DxcCreateInstance missing");
        return;
    }
    GLOG_INFO("DXC: loaded (runtime), HLSL -> DXIL compilation enabled");
}

DxcCreateInstanceProc create_instance() {
    std::call_once(g_load_flag, do_load);
    return g_create_instance;
}

std::string utf16_to_utf8(const wchar_t* w, size_t count) {
    if (!w) return {};
#ifdef _WIN32
    if (count == 0) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(count),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(count), out.data(), need,
                          nullptr, nullptr);
    return out;
#else
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        unsigned c = static_cast<unsigned>(w[i]);
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
#endif
}

std::wstring utf8_to_utf16(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                           nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
#else
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        for (int k = 0; k < extra && i + 1 < s.size(); ++k) {
            ++i;
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        out.push_back(static_cast<wchar_t>(cp));
        ++i;
    }
    return out;
#endif
}

} // namespace

const char* dxc_profile_string(DxcProfile p) {
    switch (p) {
        case DxcProfile::Vs_5_0: return "vs_5_0";
        case DxcProfile::Ps_5_0: return "ps_5_0";
        case DxcProfile::Cs_5_0: return "cs_5_0";
        case DxcProfile::Vs_6_0: return "vs_6_0";
        case DxcProfile::Ps_6_0: return "ps_6_0";
        case DxcProfile::Cs_6_0: return "cs_6_0";
        default: return "vs_6_0";
    }
}

bool dxc_available() {
    const bool ok = create_instance() != nullptr;
    (void)g_attempted;
    return ok;
}

std::string dxc_version() {
    // 不做版本查询：需要额外 IID + 调 API，日志里用处不大。
    return dxc_available() ? "runtime" : "";
}

bool compile_hlsl_to_dxil(const std::string& hlsl, const std::string& entry,
                          DxcProfile profile, std::vector<uint8_t>& out_dxil,
                          std::string& error) {
    out_dxil.clear();
    error.clear();

    DxcCreateInstanceProc create = create_instance();
    if (!create) {
        error = "dxcompiler not available";
        return false;
    }
    if (hlsl.empty()) {
        error = "empty HLSL source";
        return false;
    }

    IDxcCompiler3Local* compiler = nullptr;
    HRESULT hr = create(kClsidDxcCompiler, kIidDxcCompiler3,
                        reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || !compiler) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "DxcCreateInstance(IDxcCompiler3) failed hr=0x%08lx",
                      static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }

    const std::wstring w_entry = utf8_to_utf16(entry.empty() ? std::string("main") : entry);
    const std::string target = dxc_profile_string(profile);
    const std::wstring w_target = utf8_to_utf16(target);

    // 参数表。DXC 需要 `-T <profile>` `-E <entry>`。
    // ★ 关闭优化会显著变慢且徒增体积，保持默认 -O3；
    //   加 -Zpc 关掉行列号（体积更小、更快），本项目不做源码级调试。
    const wchar_t* args[] = {
        L"-T", w_target.c_str(),
        L"-E", w_entry.c_str(),
        L"-Zpc",
        L"-all_resources_bound",
    };
    const unsigned arg_count = static_cast<unsigned>(sizeof(args) / sizeof(args[0]));

    DxcBufferLocal buf;
    buf.ptr = hlsl.data();
    buf.size = hlsl.size();
    buf.encoding = 65001;   // DXC_CP_UTF8

    IDxcResultLocal* result = nullptr;
    hr = compiler->Compile(&buf, args, arg_count, nullptr, kIidDxcResult,
                           reinterpret_cast<void**>(&result));
    if (FAILED(hr) || !result) {
        char b[80];
        std::snprintf(b, sizeof(b), "IDxcCompiler3::Compile failed hr=0x%08lx",
                      static_cast<unsigned long>(hr));
        error = b;
        compiler->Release();
        return false;
    }

    // 取错误信息（无论成败都可能有 warning）。
    std::string diag;
    {
        void* err_obj = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS_L, kIidDxcBlob, &err_obj, nullptr)) &&
            err_obj) {
            auto* blob = static_cast<IDxcBlobLocal*>(err_obj);
            const char* p = static_cast<const char*>(blob->GetBufferPointer());
            const size_t n = blob->GetBufferSize();
            if (p && n) diag.assign(p, n);
            blob->Release();
        }
    }

    HRESULT status = 0;
    result->GetStatus(&status);
    if (FAILED(status)) {
        error = diag.empty() ? "DXC compilation failed (no diagnostics)" : diag;
        result->Release();
        compiler->Release();
        return false;
    }

    void* obj = nullptr;
    hr = result->GetOutput(DXC_OUT_OBJECT_L, kIidDxcBlob, &obj, nullptr);
    if (FAILED(hr) || !obj) {
        error = "DXC: DXC_OUT_OBJECT missing";
        result->Release();
        compiler->Release();
        return false;
    }

    auto* blob = static_cast<IDxcBlobLocal*>(obj);
    const void* p = blob->GetBufferPointer();
    const size_t n = blob->GetBufferSize();
    if (!p || n == 0) {
        error = "DXC: empty DXIL blob";
        blob->Release();
        result->Release();
        compiler->Release();
        return false;
    }
    out_dxil.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    blob->Release();
    result->Release();
    compiler->Release();

    if (!diag.empty()) {
        // 成功但有告警：记 info 级，避免噪声。
        GLOG_INFO("DXC: compiled '{}' -> {} ({} bytes), warnings: {}", entry, target,
                  out_dxil.size(), diag);
    }
    return true;
}

} // namespace gryce_engine::render::d3d12
