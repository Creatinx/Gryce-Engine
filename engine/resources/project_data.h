// ===========================================================================
// engine/resources/project_data.h
// project.data 的唯一读取入口。
//
// 为什么单独抽一层：这份文件同时被 core（启动场景、pak 密钥）和 app 宿主
// （窗口、渲染设置）解析，之前各写一份 std::ifstream + json >>，导致同一个
// 语法错误在两处各报一条、口径还不一致。更要命的是手写配置常见的尾随逗号
// 会让整个文件解析失败 —— 于是所有设置静默退回默认值，用户只看到"改了没
// 效果"。这里统一做三件事：
//   1) 容错：剥掉对象/数组末尾的多余逗号与 BOM，允许注释；
//   2) 诊断：失败时给出行列位置与"最可能是什么错"的提示；
//   3) 只读一次磁盘。
// ===========================================================================
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "export.h"

namespace gryce_engine::resources {

struct ProjectDataResult {
    /// 文件是否存在。缺失是正常情况（用内置默认值），调用方不必报错。
    bool found = false;
    /// 解析是否成功。found && !ok 表示文件存在但语法有错。
    bool ok = false;
    /// 解析结果（仅在 ok 时有效）。
    nlohmann::json data;
    /// 读取时做过的容错修正，例如"第 12 行的尾随逗号已忽略"。
    /// 非空时应当以 WARN 打出来 —— 文件能读，但作者的写法需要修正。
    std::string tolerant_note;
    /// ok == false 时的错误描述（含行列号与提示）。
    std::string error;
};

/// 读取 <root>/project.data。
/// 跨 DLL 导出：core（解析启动场景/密钥）与 app 宿主（窗口与渲染设置）都用它。
GRYCE_API ProjectDataResult read_project_data(const std::string& root);

} // namespace gryce_engine::resources
