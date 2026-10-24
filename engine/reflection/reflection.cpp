// ===========================================================================
// engine/reflection/reflection.cpp
// 反射注册表实现。类型名 / hash 口径见 reflection/type_name.h。
// ===========================================================================

#include "reflection/reflection.h"

#include "reflection/type_name.h"

namespace gryce_engine::reflection {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

// ---------------------------------------------------------------------------
// full_hash_of —— 短名 → 全名 hash
// ---------------------------------------------------------------------------
uint64_t Registry::full_hash_of(const std::string& short_name) const {
    const TypeInfo* info = find(short_name);
    if (!info) return 0;
    if (info->rtti_name.empty()) return 0;
    return type_hash_of_name(demangle_type_name(info->rtti_name.c_str()));
}

} // namespace gryce_engine::reflection
