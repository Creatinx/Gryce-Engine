// ===========================================================================
// editor/ext/bridge/js_args.cpp — JS 值读取实现
// ===========================================================================

#include "bridge/js_args.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace gryce::editor::bridge {

bool Args::number(int index, double* out) const {
    if (!out || index < 0 || index >= argc_) return false;
    double v = 0.0;
    if (!api_->arg_number(ctx_, argv_[index], &v)) return false;
    // 这里**不**过滤 NaN / Inf：那是"语义非法"，由各命令自己判并返回 0，
    // 调用方能用返回值处理。在这里拦掉会变成抛错，把"数值不合理"和
    // "参数类型写错"混成同一种表现。
    *out = v;
    return true;
}

bool Args::number(int index, float* out) const {
    double v = 0.0;
    if (!number(index, &v)) return false;
    *out = static_cast<float>(v);
    return true;
}

bool Args::boolean(int index, bool* out) const {
    if (!out || index < 0 || index >= argc_) return false;
    // arg_bool 对非布尔值返回 0，无法区分"false"和"类型不对"；
    // 这里按 JS 的真值语义处理：数字非 0 为真，其它交给 arg_bool。
    double v = 0.0;
    if (api_->arg_number(ctx_, argv_[index], &v)) {
        *out = v != 0.0;
        return true;
    }
    *out = api_->arg_bool(ctx_, argv_[index]) != 0;
    return true;
}

bool Args::str(int index, std::string* out) const {
    if (!out || index < 0 || index >= argc_) return false;
    // 路径最长 512 字节足够（Windows MAX_PATH 的常见上限），超长视为非法。
    char buf[512] = {};
    const int n = api_->arg_string(ctx_, argv_[index], buf, static_cast<int>(sizeof(buf)));
    if (n < 0) return false;
    out->assign(buf, static_cast<std::size_t>(n));
    return true;
}

bool Args::handle(int index, GEntityHandle* out) const {
    double v = 0.0;
    if (!number(index, &v)) return false;
    // NaN / Inf 也必须在这里挡：它们与任何数比较都是 false，
    // 会一路穿过范围检查，最后落到 static_cast<int> 的未定义行为上。
    if (!std::isfinite(v)) return false;
    if (v < 0.0 || v > 2147483647.0) return false;
    const double truncated = std::floor(v);
    // 小数句柄一定是调用方算错了（例如把浮点坐标当句柄传进来），
    // 静默截断会让"选了实体 3"这种问题极难定位。
    if (std::fabs(v - truncated) > 1e-9) return false;
    *out = static_cast<GEntityHandle>(truncated);
    return true;
}

bool Args::i32(int index, int* out) const {
    double v = 0.0;
    if (!number(index, &v)) return false;
    if (!std::isfinite(v)) return false;
    if (v < -2147483648.0 || v > 2147483647.0) return false;
    if (std::fabs(v - std::floor(v)) > 1e-9) return false;
    *out = static_cast<int>(v);
    return true;
}

bool Args::u64(int index, uint64_t* out) const {
    if (!out) return false;

    // 先试字符串：64 位哈希必须走这条路，double 只有 53 位精度。
    if (index >= 0 && index < argc_) {
        char buf[32] = {};
        const int n = api_->arg_string(ctx_, argv_[index], buf, static_cast<int>(sizeof(buf)));
        if (n > 0) {
            errno = 0;
            char* end = nullptr;
            const unsigned long long v = std::strtoull(buf, &end, 10);
            if (errno == 0 && end && *end == '\0') {
                *out = static_cast<uint64_t>(v);
                return true;
            }
            return false;   // 是字符串但不是一个十进制数
        }
    }

    double v = 0.0;
    if (!number(index, &v)) return false;
    if (!std::isfinite(v) || v < 0.0) return false;
    *out = static_cast<uint64_t>(v);
    return true;
}

} // namespace gryce::editor::bridge
