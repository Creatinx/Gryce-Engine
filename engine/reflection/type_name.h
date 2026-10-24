// ===========================================================================
// engine/reflection/type_name.h
// 组件类型名与类型 hash 的共享口径（跨 TU 单一真源）。
//
// 背景：编辑器侧要拿「注册类型表」里的类型与「实体上的组件」对应起来，
// 两侧必须用同一把尺子量类型。历史上引擎里有三套互不相干的 hash 口径：
//   - RTTI 解修饰全名的 std::hash<std::string>（GComponent_GetTypeHashAt）
//   - 工厂短名的 std::hash<std::string>（GComponent_GetRegisteredTypeInfo）
//   - 组件 type() 字符串的 hash（Tilemap 系列）
// 本文件把「可读全名 → hash」这一口径收敛到一处：
//   demangle_type_name()  RTTI mangled 名 → 可读全名（跨编译器解修饰）
//   type_hash_of_name()    可读全名 → 统一 hash
//
// 约定：hash 一律按「可读全名」计算。全名形如
//   gryce_engine::components::d2::basic_rect::ColorRect
// ===========================================================================

#pragma once

#include <cstdint>
#include <string>

#include "export.h"

namespace gryce_engine::reflection {

// ---------------------------------------------------------------------------
// demangle_type_name —— RTTI mangled 名 → 可读全名
//
// 输入 typeid(T).name() 的原始输出。GCC / Clang 走 abi::__cxa_demangle；
// MSVC 的 typeid(T).name() 本身就是可读名，只需剥掉 "class " / "struct "
// 前缀。解修饰失败时原样返回入参（不抛异常、不返回空串）。
// ---------------------------------------------------------------------------
GRYCE_API std::string demangle_type_name(const char* mangled);

// ---------------------------------------------------------------------------
// type_hash_of_name —— 统一的组件类型 hash
//
// 对可读全名取 std::hash<std::string>。这是引擎侧唯一的组件类型 hash 口径：
// GComponent_GetTypeHashAt / GComponent_GetRegisteredTypeInfo /
// GComponent_GetTypeHashByName 返回的都是它。
// ---------------------------------------------------------------------------
GRYCE_API uint64_t type_hash_of_name(const std::string& readable_name);

} // namespace gryce_engine::reflection
