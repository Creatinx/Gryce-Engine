// ===========================================================================
// editor/ext/bridge/js_args.h — JS 值 ↔ C 类型
//
// 扩展 ABI 只提供 arg_number / arg_string / arg_bool 三个读取函数，类型全是
// double 与 C 字符串。有两个地方必须小心：
//
//   1. uint64 的组件类型哈希超过 2^53 时，经 double 转一手会丢精度，
//      于是"查得到类型、写不进属性"。这里允许调用方传字符串（"123456789"），
//      字符串优先解析，传 number 也能用（小哈希不丢）。
//
//   2. 实体句柄在 JS 里是整数，但可能被传成 3.7 或 NaN。句柄按 int 截断前
//      必须先确认是正整数且在 int 范围内，否则 3.7 会被当成实体 3。
// ===========================================================================

#ifndef GRYCE_EDITOR_BRIDGE_JS_ARGS_H
#define GRYCE_EDITOR_BRIDGE_JS_ARGS_H

#include "api/extension_api.h"
#include "api/types.h"

#include <cstdint>
#include <string>

namespace gryce::editor::bridge {

// 一次原生调用期间有效的参数视图。不做缓冲、不做拷贝，
// argv 只在调用期间有效（ABI 约定），所以这里也只活这么久。
class Args {
public:
    Args(GryceExtensionApi* api, void* ctx, int argc, const GJsValue* argv)
        : api_(api), ctx_(ctx), argc_(argc), argv_(argv) {}

    int count() const { return argc_; }

    bool number(int index, double* out) const;
    bool number(int index, float* out) const;
    bool boolean(int index, bool* out) const;
    bool str(int index, std::string* out) const;

    // 实体句柄：正整数且 <= INT32_MAX。0 是合法值（表示"场景根 / 清空选择"），
    // 负数与小数一律判失败。
    bool handle(int index, GEntityHandle* out) const;

    // 32 位整数（轴编号、模式、键码）。
    bool i32(int index, int* out) const;

    // 64 位无符号（组件类型哈希）。字符串优先，number 兜底。
    bool u64(int index, uint64_t* out) const;

private:
    GryceExtensionApi* api_;
    void* ctx_;
    int argc_;
    const GJsValue* argv_;
};

} // namespace gryce::editor::bridge

#endif // GRYCE_EDITOR_BRIDGE_JS_ARGS_H
