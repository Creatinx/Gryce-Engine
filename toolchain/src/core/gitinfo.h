#pragma once

// ---------------------------------------------------------------------------
// GryceGC gitinfo：git 元数据（M5 meta stamp-git / VERSIONINFO 需要）
// ---------------------------------------------------------------------------

#include <string>

namespace grycegc {

// 当前仓库 HEAD 短 commit（非 git 仓库或失败返回空）
std::string git_head_short();

// HEAD 完整 commit
std::string git_head_full();

// 最近提交的 ISO 日期（%cI；失败返回空）
std::string git_head_date();

// 一次性拿到 "build_id@date" 组合（任一为空则相应部分留空）
struct GitStamp {
    std::string commit;
    std::string date;
    bool ok = false;
};
GitStamp git_stamp();

} // namespace grycegc
