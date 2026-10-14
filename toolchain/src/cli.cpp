#include "cli.h"

#include <cctype>
#include <cstring>

namespace grycegc {

namespace {

// 拆分 "--name=value" 为 ("name", "value")，无 '=' 返回 ("", original)
std::pair<std::string, std::string> split_kv(const std::string& tok) {
    auto eq = tok.find('=');
    if (eq == std::string::npos) return {{}, {}};
    return {tok.substr(0, eq), tok.substr(eq + 1)};
}

bool starts_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

} // namespace

bool Options::has(const std::string& k) const {
    return single.count(k) || all.count(k) || flags.count(k);
}
bool Options::is_flag(const std::string& k) const { return flags.count(k) > 0; }

std::string Options::get(const std::string& k, const std::string& def) const {
    auto it = single.find(k);
    if (it != single.end()) return it->second;
    auto jt = all.find(k);
    if (jt != all.end() && !jt->second.empty()) return jt->second.back();
    return def;
}

std::vector<std::string> Options::get_all(const std::string& k) const {
    auto it = all.find(k);
    if (it == all.end()) return {};
    return it->second;
}

int Options::get_int(const std::string& k, int def) const {
    auto v = get(k);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

bool parse_argv(int argc, const char* const argv[],
                std::string& command, Options& opts, std::string& error) {
    command.clear();
    opts = {};
    opts.argc = argc;
    opts.argv = argv;
    if (argc <= 1) return true;  // 无参数；main 走 help

    int i = 1;
    // 第一个非选项 token 视为命令名
    if (std::strcmp(argv[i], "--") == 0) {
        error = "无命令名（首次出现的 token 是 '--'）";
        return false;
    }
    if (argv[i][0] == '-') {
        error = std::string("未知选项：'") + argv[i] + "'（GryceGC 需要先给子命令名，"
                                                         "如：GryceGC <command> [options]）";
        return false;
    }
    command = argv[i];
    ++i;

    bool passthrough_mode = false;
    for (; i < argc; ++i) {
        const std::string tok = argv[i];
        if (passthrough_mode) {
            opts.passthrough.push_back(tok);
            continue;
        }
        if (tok == "--") {
            passthrough_mode = true;
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' && tok[1] == '-') {
            const std::string body = tok.substr(2);
            if (body.empty()) {
                // "--" 已在上方处理；裸 "--" 不应到这里
                error = "空选项 '--'";
                return false;
            }
            auto kv = split_kv(body);
            if (!kv.first.empty()) {
                // --name=value
                opts.single[kv.first] = kv.second;
                opts.all[kv.first].push_back(kv.second);
                continue;
            }
            // --name ；若下一 token 不以 '-' 起头则合并
            if (i + 1 < argc) {
                const std::string nxt = argv[i + 1];
                if (!nxt.empty() && nxt[0] != '-') {
                    opts.single[body] = nxt;
                    opts.all[body].push_back(nxt);
                    ++i;
                    continue;
                }
            }
            opts.flags.insert(body);
            continue;
        }
        if (!tok.empty() && tok[0] == '-') {
            // 负数（"-5"、"-0.5"）等数值 token 按 positional 处理，
            // 而不是当作未知短选项报错
            const bool numeric =
                tok.size() > 1 && (std::isdigit(static_cast<unsigned char>(tok[1])) ||
                                   tok[1] == '.');
            if (!numeric) {
                error = std::string("未知短选项：'") + tok + "'（仅支持 --name / --name=value 形式）";
                return false;
            }
        }
        opts.positionals.push_back(tok);
    }
    return true;
}

} // namespace grycegc
