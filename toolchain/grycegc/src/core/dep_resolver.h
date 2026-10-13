#pragma once

// ---------------------------------------------------------------------------
// GryceGC dep_resolver：解析从可执行文件出发的完整 DLL 依赖闭包
//
// 移植自 toolchain/resource.py 的 DepResolver，差异：
//   - 移除 objdump 路径依赖（统一走 pe_image::import_dll_names 自解析）
//   - 路径走 std::filesystem，原生支持跨平台
//   - 返回结构化的 DepNode 列表，命令层只负责格式化与分类汇总
// ---------------------------------------------------------------------------

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace grycegc {

struct DepNode {
    std::string name_lower;       // 导入表中的 dll 名（小写）
    std::string name_orig;        // 原始大小写（文件内写法）
    std::filesystem::path source; // 闭包内找到的来源（缺失时为空）
};

struct DepResolveResult {
    std::vector<DepNode> found;   // 找到的（按 BFS 顺序）
    std::vector<std::string> missing;  // 闭包内未找到的（lower 名）
};

// 分类：core（DLL 名以 gryce 开头或匹配引擎三 DLL）→ 与 exe 同级；其余 → 依赖
enum class DepCategory { Core, Dependency };
DepCategory classify(const std::string& name_lower);

// 已知系统 DLL 集合（M1 与 Python 版一致；将来从 SYSTEM_DLLS 同步扩展）
bool is_system_dll(const std::string& name_lower);

// 常见 MinGW bin 自动探测
std::vector<std::filesystem::path> discover_mingw_bins();

class DepResolver {
public:
    DepResolver(std::filesystem::path build_dir,
                std::vector<std::filesystem::path> extra_dirs = {});

    // 把 PATH 目录（顶层 .dll）也加入候选（仅最上层，匹配原 Python 行为）
    void include_path_env(bool yes = true);

    // 闭包求解：从 entry 文件出发，沿其导入表 BFS 展开
    DepResolveResult resolve(const std::filesystem::path& entry) const;

private:
    std::filesystem::path build_dir_;
    std::vector<std::filesystem::path> extra_dirs_;
    bool include_path_ = true;

    mutable bool name_map_ready_ = false;
    mutable std::map<std::string, std::filesystem::path> name_map_;
    void build_name_map() const;
};

} // namespace grycegc
