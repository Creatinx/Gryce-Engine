#include "core/gitinfo.h"

#include "core/fsutil.h"
#include "core/process.h"

#include <string>

namespace grycegc {

namespace {

std::string git_query(const std::string& args) {
    // Windows CreateProcessW 不搜 PATH，先解析 git 绝对路径
    std::string git = which("git");
    if (git.empty()) {
        git = "C:/Program Files/Git/bin/git.exe";
        if (!std::filesystem::exists(git)) return {};
    }
    // 按空格拆 argv（git 子命令参数不含带空格的值）
    std::vector<std::string> argv;
    size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && args[i] == ' ') ++i;
        size_t j = args.find(' ', i);
        if (j == std::string::npos) j = args.size();
        if (i < j) argv.push_back(args.substr(i, j - i));
        i = j;
    }
    const auto r = run_process_capture(git, argv, {});
    if (!r.spawned) return {};
    std::string s = r.stdout_text;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

std::string git_head_short() { return git_query("rev-parse --short HEAD"); }
std::string git_head_full() { return git_query("rev-parse HEAD"); }
std::string git_head_date() { return git_query("log -1 --format=%cI"); }

GitStamp git_stamp() {
    GitStamp st;
    st.commit = git_head_full();
    st.date = git_head_date();
    st.ok = !st.commit.empty();
    return st;
}

} // namespace grycegc
