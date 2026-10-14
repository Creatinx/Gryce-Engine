#include "core/process.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <vector>
#endif

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <algorithm>
#endif

namespace grycegc {

namespace {

std::string join_args_win(const std::string& exe, const std::vector<std::string>& args) {
    // CreateProcessW 的 lpCommandLine 需自行保证引号语义：
    // exe 路径一律加引号（PATH 常含空格，如 C:/Program Files/...）；
    // 参数含空格/制表符时加引号（内部用法参数均受控，此处保守处理）。
    std::string s = "\"" + exe + "\"";
    for (const auto& a : args) {
        s += ' ';
        if (a.find(' ') != std::string::npos || a.find('\t') != std::string::npos) {
            s += "\"" + a + "\"";
        } else {
            s += a;
        }
    }
    return s;
}

#if defined(_WIN32)

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 构造新的环境块（UTF-16，\0 分隔，双 \0 结尾）
std::wstring build_env_block(const EnvOverrides& env) {
    // 拷贝当前环境（一次获取，迭代完统一释放）
    std::vector<std::wstring> pairs;
    if (wchar_t* raw = GetEnvironmentStringsW()) {
        for (wchar_t* p = raw; *p; ) {
            std::wstring kv(p);
            pairs.push_back(kv);
            p += kv.size() + 1;
        }
        FreeEnvironmentStringsW(raw);
    }

    for (const auto& [name, value] : env) {
        const std::wstring wname = to_wide(name);
        if (value.empty()) {  // 空值 = 删除
            pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                       [&](const std::wstring& kv) {
                                           return kv.rfind(wname + L"=", 0) == 0;
                                       }),
                        pairs.end());
            continue;
        }
        bool replaced = false;
        const std::wstring prefix = wname + L"=";
        for (auto& kv : pairs) {
            if (kv.rfind(prefix, 0) == 0) { kv = prefix + to_wide(value); replaced = true; break; }
        }
        if (!replaced) pairs.push_back(prefix + to_wide(value));
    }

    std::wstring block;
    for (const auto& kv : pairs) { block += kv; block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

ProcessResult spawn_win(const fs::path& exe, const std::vector<std::string>& args,
                        const EnvOverrides& env, bool capture) {
    ProcessResult r;
    std::wstring cmdline = to_wide(join_args_win(exe.string(), args));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (capture) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
            r.spawn_error = "CreatePipe failed";
            return r;
        }
        SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = pipe_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::wstring env_block = build_env_block(env);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.wstring().c_str(), cmdline.data(), nullptr, nullptr,
                                   capture ? TRUE : FALSE,
                                   CREATE_UNICODE_ENVIRONMENT, env_block.empty() ? nullptr :
                                   env_block.data(), nullptr, &si, &pi);
    if (capture) CloseHandle(pipe_write);

    if (!ok) {
        if (pipe_read) CloseHandle(pipe_read);
        r.spawn_error = "CreateProcessW failed (GetLastError=" +
                        std::to_string(GetLastError()) + ")";
        return r;
    }

    if (capture) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(pipe_read, buf, sizeof(buf), &got, nullptr) && got > 0) {
            r.stdout_text.append(buf, got);
        }
        CloseHandle(pipe_read);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.spawned = true;
    r.exit_code = static_cast<int>(code);
    return r;
}

#else // POSIX

std::vector<std::string> build_env_vector(const EnvOverrides& env) {
    std::vector<std::string> pairs;
    for (char** p = environ; *p; ++p) pairs.emplace_back(*p);
    for (const auto& [name, value] : env) {
        const std::string prefix = name + "=";
        if (value.empty()) {
            pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                       [&](const std::string& kv) {
                                           return kv.rfind(prefix, 0) == 0;
                                       }),
                        pairs.end());
            continue;
        }
        bool replaced = false;
        for (auto& kv : pairs) {
            if (kv.rfind(prefix, 0) == 0) { kv = prefix + value; replaced = true; break; }
        }
        if (!replaced) pairs.push_back(prefix + value);
    }
    return pairs;
}

ProcessResult spawn_posix(const fs::path& exe, const std::vector<std::string>& args,
                          const EnvOverrides& env, bool /*capture*/) {
    ProcessResult r;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> env_pairs = build_env_vector(env);
    std::vector<char*> envp;
    for (auto& kv : env_pairs) envp.push_back(const_cast<char*>(kv.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        r.spawn_error = "fork failed";
        return r;
    }
    if (pid == 0) {
        execve(exe.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    r.spawned = true;
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return r;
}

#endif

} // namespace

std::string prepend_to_path_env(const std::string& dir, const std::string& old_path) {
#if defined(_WIN32)
    return dir + ";" + old_path;
#else
    return dir + ":" + old_path;
#endif
}

ProcessResult run_process(const fs::path& exe, const std::vector<std::string>& args,
                          const EnvOverrides& env) {
#if defined(_WIN32)
    return spawn_win(exe, args, env, /*capture=*/false);
#else
    return spawn_posix(exe, args, env, /*capture=*/false);
#endif
}

ProcessResult run_process_capture(const fs::path& exe, const std::vector<std::string>& args,
                                  const EnvOverrides& env) {
#if defined(_WIN32)
    return spawn_win(exe, args, env, /*capture=*/true);
#else
    // POSIX 简化：不重定向，返回空 stdout（bench 目前仅支持 Windows 构建树）
    ProcessResult r = spawn_posix(exe, args, env, /*capture=*/true);
    return r;
#endif
}

} // namespace grycegc
