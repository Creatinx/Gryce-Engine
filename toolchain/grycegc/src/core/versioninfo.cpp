#include "core/versioninfo.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <vector>

namespace grycegc {
namespace {

// 把 UTF-8 转 UTF-16 静态块（用于 VS_FIXEDFILEINFO 之外的字符串）
std::wstring w(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
    return r;
}

void append_wstr(std::vector<uint8_t>& buf, const std::wstring& s) {
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(s.data()),
               reinterpret_cast<const uint8_t*>(s.data()) + s.size() * 2);
    const wchar_t z = 0;
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&z),
               reinterpret_cast<const uint8_t*>(&z) + 2);
}

void append_cstr(std::vector<uint8_t>& buf, const char* s) {
    const size_t n = std::strlen(s);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(s),
               reinterpret_cast<const uint8_t*>(s) + n + 1);
}

// 手工组装 VS_VERSION_INFO（英文 040904b0 语言块）
std::vector<uint8_t> build_version_resource(const VersionInfoFields& f) {
    const std::wstring wcompany = w(f.company);
    const std::wstring wcopyright = w(f.copyright);
    const std::wstring wproduct = w(f.product);
    const std::wstring wver = w(f.version);

    // --- StringFileInfo 子块 -------------------------------------------------
    // 每个 StringTable: header(6 words)+key(L"040904b0")+pairs
    // String 结构: wLength, wValueLength, wType, szKey, Value(含\0)，按 4 字节对齐
    struct StrPair { const char* key; const std::wstring* val; };
    const std::vector<StrPair> pairs = {
        {"CompanyName", &wcompany},
        {"FileDescription", &wproduct},
        {"FileVersion", &wver},
        {"InternalName", &wproduct},
        {"LegalCopyright", &wcopyright},
        {"OriginalFilename", nullptr},
        {"ProductName", &wproduct},
        {"ProductVersion", &wver},
    };

    // 先构造每个 String 的字节，再套 StringTable，再套 StringFileInfo
    std::vector<uint8_t> table_children;
    for (const auto& p : pairs) {
        std::vector<uint8_t> one;
        // 占位 wLength/wValueLength/wType（之后回填）
        one.resize(6, 0);
        append_cstr(one, p.key);
        if (p.val) {
            append_wstr(one, *p.val);
        } else {
            const wchar_t z = 0;
            one.insert(one.end(), reinterpret_cast<const uint8_t*>(&z),
                       reinterpret_cast<const uint8_t*>(&z) + 2);
        }
        while (one.size() % 4) one.push_back(0);
        *reinterpret_cast<uint16_t*>(one.data()) = static_cast<uint16_t>(one.size());
        *reinterpret_cast<uint16_t*>(one.data() + 2) =
            static_cast<uint16_t>((p.val ? p.val->size() + 1 : 1) * 2);
        *reinterpret_cast<uint16_t*>(one.data() + 4) = 1;  // text
        table_children.insert(table_children.end(), one.begin(), one.end());
    }

    std::vector<uint8_t> string_file_info;
    string_file_info.resize(6, 0);
    append_wstr(string_file_info, L"StringFileInfo");
    string_file_info.insert(string_file_info.end(), table_children.begin(),
                            table_children.end());
    while (string_file_info.size() % 4) string_file_info.push_back(0);
    *reinterpret_cast<uint16_t*>(string_file_info.data()) =
        static_cast<uint16_t>(string_file_info.size());
    *reinterpret_cast<uint16_t*>(string_file_info.data() + 2) = 0;
    *reinterpret_cast<uint16_t*>(string_file_info.data() + 4) = 1;

    // --- VarFileInfo（翻译表 040904b0）--------------------------------------
    std::vector<uint8_t> var;
    var.resize(6, 0);
    append_wstr(var, L"Translation");
    const uint32_t lang = 0x04B00409u;  // 0409 英文 US / 04B0 UTF-16（小端 0409,04B0）
    var.insert(var.end(), reinterpret_cast<const uint8_t*>(&lang),
               reinterpret_cast<const uint8_t*>(&lang) + 4);
    while (var.size() % 4) var.push_back(0);
    *reinterpret_cast<uint16_t*>(var.data()) = static_cast<uint16_t>(var.size());
    *reinterpret_cast<uint16_t*>(var.data() + 2) = 4;
    *reinterpret_cast<uint16_t*>(var.data() + 4) = 0;  // binary

    std::vector<uint8_t> var_file_info;
    var_file_info.resize(6, 0);
    append_wstr(var_file_info, L"VarFileInfo");
    var_file_info.insert(var_file_info.end(), var.begin(), var.end());
    while (var_file_info.size() % 4) var_file_info.push_back(0);
    *reinterpret_cast<uint16_t*>(var_file_info.data()) =
        static_cast<uint16_t>(var_file_info.size());
    *reinterpret_cast<uint16_t*>(var_file_info.data() + 2) = 0;
    *reinterpret_cast<uint16_t*>(var_file_info.data() + 4) = 1;

    // --- 顶层 VS_VERSION_INFO -------------------------------------------------
    std::vector<uint8_t> root;
    root.resize(6 + sizeof(VS_FIXEDFILEINFO), 0);
    append_wstr(root, L"VS_VERSION_INFO");
    root.insert(root.end(), string_file_info.begin(), string_file_info.end());
    root.insert(root.end(), var_file_info.begin(), var_file_info.end());
    while (root.size() % 4) root.push_back(0);
    *reinterpret_cast<uint16_t*>(root.data()) = static_cast<uint16_t>(root.size());
    *reinterpret_cast<uint16_t*>(root.data() + 2) = sizeof(VS_FIXEDFILEINFO);
    *reinterpret_cast<uint16_t*>(root.data() + 4) = 0;  // binary
    auto* ffi = reinterpret_cast<VS_FIXEDFILEINFO*>(root.data() + 6);
    ffi->dwSignature = 0xFEEF04BD;
    ffi->dwFileVersionMS = 1u << 16;
    ffi->dwFileVersionLS = 0;
    ffi->dwProductVersionMS = 1u << 16;
    ffi->dwProductVersionLS = 0;
    return root;
}

} // namespace

bool pe_write_version_info(const fs::path& pe, const VersionInfoFields& f) {
    const std::vector<uint8_t> res = build_version_resource(f);
    std::wstring wpe = pe.wstring();  // BeginUpdateResourceW 需要非 const LPWSTR
    // MinGW 的 MAKEINTRESOURCEW 在部分版本返回 LPSTR，直接强转避免签名不匹配
    const wchar_t* kResId = reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(1));
    HANDLE h = BeginUpdateResourceW(wpe.data(), FALSE);
    if (!h) return false;
    const BOOL ok = UpdateResourceW(h, kResId, kResId,
                                    MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                    const_cast<void*>(static_cast<const void*>(res.data())),
                                    static_cast<DWORD>(res.size()));
    if (!ok) {
        EndUpdateResourceW(h, TRUE);
        return false;
    }
    return EndUpdateResourceW(h, FALSE) != FALSE;
}

} // namespace grycegc

#else // !_WIN32

namespace grycegc {

bool pe_write_version_info(const fs::path&, const VersionInfoFields&) {
    return true;  // no-op
}

} // namespace grycegc

#endif
