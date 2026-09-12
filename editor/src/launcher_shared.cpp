#include "launcher_shared.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <unistd.h>
#include <climits>
#endif

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace gryce_engine::editor::launcher {

std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buffer).parent_path();
    }
#else
    char buffer[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        return fs::path(buffer).parent_path();
    }
#endif
    return fs::current_path();
}

std::filesystem::path config_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0]) {
        return fs::path(appdata) / "GryceEngine" / "launcher.json";
    }
#endif
    return executable_dir() / "launcher.json";
}

std::vector<std::string> load_recent() {
    std::vector<std::string> out;
    std::ifstream in(config_path());
    if (!in.is_open()) return out;
    try {
        json j = json::parse(in);
        if (j.contains("recent") && j["recent"].is_array()) {
            for (const auto& item : j["recent"]) {
                if (item.is_string()) out.push_back(item.get<std::string>());
            }
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

void save_recent(const std::vector<std::string>& recent) {
    json j;
    j["recent"] = recent;
    // 保留已有 settings（仅在确实为对象时，避免 settings 为 null/text 时 value() 抛 306）
    json settings = json::object();
    std::ifstream in(config_path());
    if (in.is_open()) {
        try {
            json existing = json::parse(in);
            if (existing.contains("settings") && existing["settings"].is_object()) {
                settings = existing["settings"];
            }
        } catch (...) {}
    }
    j["settings"] = settings;
    std::error_code ec;
    fs::create_directories(config_path().parent_path(), ec);
    std::ofstream out(config_path());
    if (out.is_open()) {
        out << j.dump(2);
    }
}

void push_recent(std::vector<std::string>& recent, const std::string& path) {
    auto it = std::find(recent.begin(), recent.end(), path);
    if (it != recent.end()) recent.erase(it);
    recent.insert(recent.begin(), path);
    if (recent.size() > 12) recent.resize(12);
    save_recent(recent);
}

LauncherSettings load_settings() {
    LauncherSettings s;
    std::ifstream in(config_path());
    if (!in.is_open()) return s;
    try {
        json j = json::parse(in);
        if (j.contains("settings") && j["settings"].is_object()) {
            const auto& st = j["settings"];
            if (st.contains("defaultDir") && st["defaultDir"].is_string())
                s.defaultDir = st["defaultDir"].get<std::string>();
            if (st.contains("editorArgs") && st["editorArgs"].is_string())
                s.editorArgs = st["editorArgs"].get<std::string>();
            if (st.contains("lastVersion") && st["lastVersion"].is_string())
                s.lastVersion = st["lastVersion"].get<std::string>();
        }
    } catch (...) {}
    return s;
}

void save_settings(const LauncherSettings& settings) {
    json j;
    json merged;
    std::ifstream in(config_path());
    if (in.is_open()) {
        try {
            json existing = json::parse(in);
            if (existing.contains("recent")) merged["recent"] = existing["recent"];
        } catch (...) {}
    }
    json st;
    st["defaultDir"] = settings.defaultDir;
    st["editorArgs"] = settings.editorArgs;
    st["lastVersion"] = settings.lastVersion;
    merged["settings"] = st;
    std::error_code ec;
    fs::create_directories(config_path().parent_path(), ec);
    std::ofstream out(config_path());
    if (out.is_open()) {
        out << merged.dump(2);
    }
}

#ifdef _WIN32
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string pick_folder() {
    std::string result;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return result;

    IFileDialog* dialog = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr)) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = wide_to_utf8(path ? path : L"");
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    CoUninitialize();
    return result;
}
#else
std::string pick_folder() {
    return {};
}
#endif

std::string format_file_time(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return {};
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

bool launch_editor(const std::string& project_path, std::string& error,
                   const std::string& extra_args) {
    const fs::path editor_exe = executable_dir() / "GryceEditor.exe";
    if (!fs::exists(editor_exe)) {
        error = "GryceEditor.exe not found next to launcher: " +
                editor_exe.string();
        return false;
    }
#ifdef _WIN32
    std::wstring exe_w = editor_exe.wstring();
    std::wstring project_w = fs::u8path(project_path).wstring();
    std::wstring extra_w = fs::u8path(extra_args).wstring();
    std::wstring cmd = L"\"" + exe_w + L"\" --project \"" + project_w + L"\"";
    if (!extra_w.empty()) {
        cmd += L" " + extra_w;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe_w.c_str(), const_cast<wchar_t*>(cmd.c_str()),
                             nullptr, nullptr, FALSE,
                             CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, editor_exe.parent_path().wstring().c_str(),
                             &si, &pi);
    if (!ok) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "CreateProcess failed (code %lu)",
                      static_cast<unsigned long>(GetLastError()));
        error = buf;
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        execl(editor_exe.string().c_str(), editor_exe.string().c_str(),
              "--project", project_path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

bool create_project_dir(const std::string& name, const std::string& parent,
                        std::string& error, fs::path& out_dir) {
    if (name.empty()) {
        error = "项目名称不能为空";
        return false;
    }
    if (parent.empty()) {
        error = "项目位置不能为空";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) {
        error = "目录不存在: " + parent;
        return false;
    }
    const fs::path project_dir = fs::path(parent) / name;
    if (fs::exists(project_dir, ec)) {
        error = "目录已存在: " + project_dir.string();
        return false;
    }
    if (!fs::create_directories(project_dir / "scenes", ec)) {
        error = "创建目录失败: " + ec.message();
        return false;
    }
    out_dir = project_dir;
    return true;
}

} // namespace gryce_engine::editor::launcher