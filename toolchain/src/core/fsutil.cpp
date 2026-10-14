#include "fsutil.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <lmcons.h>  // UNLEN
#else
#include <unistd.h>
#include <climits>
#endif

namespace grycegc {

bool copy_tree(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (ec) return false;
    for (auto it = std::filesystem::recursive_directory_iterator(
             src, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& from = it->path();
        const auto rel = std::filesystem::relative(from, src, ec);
        if (ec) continue;
        const auto to = dst / rel;
        if (it->is_directory(ec)) {
            std::filesystem::create_directories(to, ec);
        } else if (it->is_regular_file(ec)) {
            std::filesystem::create_directories(to.parent_path(), ec);
            std::filesystem::copy_file(from, to,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
        ec.clear();
    }
    return true;
}

bool write_binary(const std::filesystem::path& p, const void* data, size_t len) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return f.good();
}

std::vector<uint8_t> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (sz <= 0) return {};
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    if (!f.good()) return {};
    return out;
}

std::string posix_path(const std::filesystem::path& p) {
    std::string s = p.generic_string();
    // generic_string 已经把分隔符转 '/'
    return s;
}

std::string which(const std::string& name) {
#if defined(_WIN32)
    const char sep = ';';
    const char* ps = std::getenv("PATH");
#else
    const char sep = ':';
    const char* ps = std::getenv("PATH");
#endif
    if (!ps) return {};
    std::string p(ps);
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(sep, i);
        if (j == std::string::npos) j = p.size();
        std::string dir = p.substr(i, j - i);
        if (!dir.empty()) {
            std::filesystem::path cand = std::filesystem::path(dir) / name;
            std::error_code ec;
            if (std::filesystem::is_regular_file(cand, ec)) return cand.string();
        }
        i = j + 1;
    }
    return {};
}

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

std::string user_host() {
    std::string user;
#if defined(_WIN32)
    char buf[UNLEN + 1] = {};
    DWORD n = UNLEN + 1;
    if (GetUserNameA(buf, &n)) user = buf;
#else
    if (const char* u = std::getenv("USER")) user = u;
#endif
    std::string host;
    char hbuf[256] = {};
#if defined(_WIN32)
    DWORD hn = sizeof(hbuf);
    if (GetComputerNameA(hbuf, &hn)) host = hbuf;
#else
    if (gethostname(hbuf, sizeof(hbuf) - 1) == 0) host = hbuf;
#endif
    if (user.empty()) user = "unknown";
    if (host.empty()) host = "unknown";
    return user + "@" + host;
}

std::filesystem::path temp_dir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetTempPathA(sizeof(buf), buf) > 0) return buf;
    return std::filesystem::temp_directory_path();
#else
    if (const char* t = std::getenv("TMPDIR")) return t;
    return std::filesystem::temp_directory_path();
#endif
}

} // namespace grycegc
