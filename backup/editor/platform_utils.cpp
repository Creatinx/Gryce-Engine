#include "platform_utils.h"

#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace gryce_engine::editor {

std::filesystem::path utf8_path(const std::string& utf8) {
#ifdef _WIN32
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len > 1) {
        std::wstring wide(static_cast<size_t>(len - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
        return std::filesystem::path(wide);
    }
#endif
    return std::filesystem::path(utf8);
}

std::string path_to_utf8(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring wide = p.wstring();
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
#else
    return p.string();
#endif
}

void reveal_in_file_explorer(const std::filesystem::path& path) {
#ifdef _WIN32
    // explorer /select,"path" 异步打开文件管理器并选中目标；不等待退出。
    std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#else
    (void)path; // 目前编辑器仅支持 Windows 平台
#endif
}

std::string format_file_time(const std::filesystem::path& path) {
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return {};

    // file_clock → system_clock 的便携换算（clock_cast 并非所有标准库都可用）
    const auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sys_time);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string format_file_size(std::uintmax_t bytes) {
    if (bytes >= 1024 * 1024) {
        return std::format("{:.2f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024) {
        return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{} B", bytes);
}

} // namespace gryce_engine::editor
