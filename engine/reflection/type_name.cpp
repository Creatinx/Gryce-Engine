// ===========================================================================
// engine/reflection/type_name.cpp
// 组件类型名与类型 hash 的共享口径实现。口径说明见 type_name.h。
// ===========================================================================

#include "reflection/type_name.h"

#include <functional>

#if defined(__GNUC__) || defined(__clang__)
    #include <cstdlib>
    #include <cxxabi.h>
#endif

namespace gryce_engine::reflection {

// ---------------------------------------------------------------------------
// demangle_type_name
// ---------------------------------------------------------------------------
std::string demangle_type_name(const char* mangled) {
    if (!mangled || mangled[0] == '\0') return std::string();

#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string out(demangled);
        std::free(demangled);
        return out;
    }
    // 解修饰失败：退回到原始 mangled 名（保证调用方总能拿到非空串）
    return std::string(mangled);
#else
    // MSVC：typeid(T).name() 已经是可读名，只需剥掉 "class " / "struct " 前缀
    std::string out(mangled);
    if (out.rfind("class ", 0) == 0) {
        out.erase(0, 6);
    } else if (out.rfind("struct ", 0) == 0) {
        out.erase(0, 7);
    } else if (out.rfind("enum ", 0) == 0) {
        out.erase(0, 5);
    }
    return out;
#endif
}

// ---------------------------------------------------------------------------
// type_hash_of_name
// ---------------------------------------------------------------------------
uint64_t type_hash_of_name(const std::string& readable_name) {
    return std::hash<std::string>{}(readable_name);
}

} // namespace gryce_engine::reflection
