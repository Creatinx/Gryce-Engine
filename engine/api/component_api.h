#ifndef GRYCE_COMPONENT_API_H
#define GRYCE_COMPONENT_API_H

#include "types.h"

#ifdef _WIN32
    #ifdef GRYCE_CORE_BUILDING
        #define GRYCE_CORE_API __declspec(dllexport)
    #else
        #define GRYCE_CORE_API __declspec(dllimport)
        #ifdef _MSC_VER
            #ifdef _DEBUG
                #pragma comment(lib, "GryceCored.lib")
            #else
                #pragma comment(lib, "GryceCore.lib")
            #endif
        #endif
    #endif
#else
    #define GRYCE_CORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

GRYCE_CORE_API int GComponent_GetCount(GEntityHandle entity);
// 组件下标口径：可见组件序列（已过滤 Transform / ParentComponent /
// ChildrenComponent）。下列所有带 index 入参的接口共用这一下标空间。
GRYCE_CORE_API int GComponent_GetTypeHashAt(GEntityHandle entity, int index, uint64_t* out_hash);
GRYCE_CORE_API int GComponent_GetTypeNameAt(GEntityHandle entity, int index, char* out_buf, int buf_size);

// 下面四个接口的 comp_type_hash 一律是「全名 hash」（见文末口径说明），
// 且支持基类查询：传基类的 hash 能命中派生出来的组件实例。
GRYCE_CORE_API int GComponent_GetPropertyCount(GEntityHandle entity, uint64_t comp_type_hash);
GRYCE_CORE_API int GComponent_GetPropertyInfo(GEntityHandle entity, uint64_t comp_type_hash, int prop_index,
                                               char* out_name, int name_buf_size,
                                               int* out_type, int* out_size);
GRYCE_CORE_API int GComponent_GetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                                           void* out_value, int value_size);
GRYCE_CORE_API int GComponent_SetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                                           const void* value, int value_size);

// ---------------------------------------------------------------------------
// Tilemap 瓦片数据（tiles 向量不在反射标量范围内，提供专用数组接口）
// comp_type_hash 是 Tilemap 类型的全名 hash。
// ---------------------------------------------------------------------------
// 读取瓦片数组到 out_tiles（最多 max_count 个）。返回实际写入的数量；失败返回 -1。
GRYCE_CORE_API int GComponent_TilemapGetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                                              int* out_tiles, int max_count);
// 写入瓦片数组（count 为 -1 时清空）。返回 0 成功，-1 失败。
GRYCE_CORE_API int GComponent_TilemapSetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                                              const int* tiles, int count);

// 加 / 删组件：comp_type_hash 是目标类型的全名 hash
//（用 GComponent_GetRegisteredTypeInfo 或 GComponent_GetTypeHashByName 取）。
// 返回 0 成功（命令已入队），-1 失败（类型未知 / 命令缓冲满）。
GRYCE_CORE_API int GComponent_AddComponent(GEntityHandle entity, uint64_t comp_type_hash);
GRYCE_CORE_API int GComponent_RemoveComponent(GEntityHandle entity, uint64_t comp_type_hash);

// 给实体（无则加）MeshRenderer 组件并设置网格路径与材质颜色。
// mesh_path 可为空（仅改颜色，要求已存在 MeshRenderer）。
// 直接操作内部 MeshRenderer::material（嵌套对象，反射标量无法触达）。
// 返回 0 成功，-1 失败（实体不存在 / 场景未初始化）。
GRYCE_CORE_API int GComponent_MeshSetMaterial(GEntityHandle entity, const char* mesh_path,
                                              float r, float g, float b,
                                              float roughness, float metallic);

// 按预设名设置 MeshRenderer 的渲染材质（预设表见 render/material_presets.h）。
// 成功返回 0；预设名不存在返回 -1（材质保持原状）。缺少 MeshRenderer 时自动创建。
GRYCE_CORE_API int GComponent_MeshSetMaterialPreset(GEntityHandle entity,
                                                    const char* preset_name);

// 读取实体 MeshRenderer 组件的网格路径到 out_buf（可空终止）。返回 0 成功，-1 失败
//（实体不存在 / 无 MeshRenderer / 路径为空）。
GRYCE_CORE_API int GComponent_MeshGetPath(GEntityHandle entity, char* out_buf, int buf_size);

// 读取实体 MeshRenderer 嵌套材质的 PBR 参数（alb/rough/metal）。返回 0 成功，-1 失败
//（实体不存在 / 无 MeshRenderer / 材质未初始化）。
GRYCE_CORE_API int GComponent_MeshGetMaterial(GEntityHandle entity,
                                              float* r, float* g, float* b,
                                              float* roughness, float* metallic);

GRYCE_CORE_API int GComponent_GetRegisteredTypeCount(void);
GRYCE_CORE_API int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size);
GRYCE_CORE_API int GComponent_GetRegisteredTypeCategory(int index, char* out_category, int category_buf_size);

// ---------------------------------------------------------------------------
// 类型 hash 口径
//
// 引擎内所有组件类型 hash 一律是「可读全名的 std::hash<std::string>>」，
// 例如 "gryce_engine::components::d2::basic_rect::ColorRect"。
// GComponent_GetTypeHashAt / GComponent_GetRegisteredTypeInfo /
// GComponent_GetTypeHashByName 返回的都是它，可以直接互相比较。
//
// 例外：工厂注册了但反射表没有的类型（ParentComponent / ChildrenComponent /
// SubViewport）回退为「短名 hash」，见 GComponent_GetRegisteredTypeInfo 注释。
// ---------------------------------------------------------------------------

// 按类型名取全名 hash。type_full_name 可以是：
//   - 反射表短名（"ColorRect"）→ 该类型的全名 hash
//   - 工厂注册名（"Script"）→ 该类型的全名 hash
//   - 可读全名（"gryce_engine::components::d2::basic_rect::ColorRect"）→ 直接算 hash
// 成功返回 0；参数非法返回 -1。
GRYCE_CORE_API int GComponent_GetTypeHashByName(const char* type_full_name, uint64_t* out_hash);

// 按全名 hash 取类型的可读全名（与 GComponent_GetTypeNameAt 同一形态）。
// 返回写入的字符数；hash 不认识（反射表里没有的类型）返回 -1。
GRYCE_CORE_API int GComponent_GetTypeNameByHash(uint64_t type_hash, char* out_buf, int buf_size);

// ---------------------------------------------------------------------------
// 组件树（实例级父子挂载）与类型继承（基类 / 子类）
//
// 两轴正交：基类/子类说的是类型继承，父/子组件说的是实例挂载。
// 一个 ColorRect 既可以是 BasicRect 的子类，也可以是某个 Sprite2D 的子组件。
//
// 【组件下标口径】所有接口的 index 都落在同一个下标空间：实体上的可见组件
// 序列（已过滤 Transform / ParentComponent / ChildrenComponent 三个内部
// 组件）。编辑器与脚本层按下标取组件时必须用同一口径，否则会错位。
// ---------------------------------------------------------------------------

// 取组件的父组件下标。没有父组件（组件树根）或下标非法时返回 -1。
GRYCE_CORE_API int GComponent_GetParentAt(GEntityHandle entity, int index);

// 设置组件的父组件。parent_index = -1 表示把该组件变成组件树根。
// 成功返回 1，失败返回 0（下标非法 / 挂到自己下面 / 会成环）。
GRYCE_CORE_API int GComponent_SetParentAt(GEntityHandle entity, int index, int parent_index);

// 取组件的子组件个数。下标非法时返回 -1。
GRYCE_CORE_API int GComponent_GetChildCountAt(GEntityHandle entity, int index);

// 取组件的第 child_index 个子组件的下标（同一可见下标空间）。
// 下标非法时返回 -1。
GRYCE_CORE_API int GComponent_GetChildAt(GEntityHandle entity, int index, int child_index);

// 生命周期继承：自身与整条父链都 enabled 才算有效。
// 返回 1 有效 / 0 无效（下标非法时也返回 0）。
GRYCE_CORE_API int GComponent_IsEffectivelyEnabledAt(GEntityHandle entity, int index);

// 取组件的局部变换（相对父组件；根组件相对宿主实体）。
// out_pos3 = [x,y,z]，out_quat4 = [x,y,z,w]，out_scale3 = [x,y,z]。
// 成功返回 0，失败返回 -1。
GRYCE_CORE_API int GComponent_GetLocalTransformAt(GEntityHandle entity, int index,
                                                  float* out_pos3, float* out_quat4, float* out_scale3);

// 设置组件的局部变换。参数含义同 GComponent_GetLocalTransformAt。
// 成功返回 1，失败返回 0。
GRYCE_CORE_API int GComponent_SetLocalTransformAt(GEntityHandle entity, int index,
                                                  const float* pos3, const float* quat4,
                                                  const float* scale3);

// 判断组件的类型是否派生自 base_type_hash 指定的类型（基类查询）。
// base_type_hash 用全名 hash 口径（GComponent_GetTypeHashByName 取）。
// 返回 1 是派生关系 / 0 不是（含下标非法）。
GRYCE_CORE_API int GComponent_IsDerivedFromAt(GEntityHandle entity, int index, uint64_t base_type_hash);

// 取组件的类型祖先链（根 → 派生，含自身），每项是全名 hash。
// 最多写入 max_count 个；返回实际写入的个数（下标非法返回 -1）。
// out_hashes 传 nullptr 时只返回链长（供调用方分配缓冲）。
GRYCE_CORE_API int GComponent_GetTypeChainAt(GEntityHandle entity, int index,
                                             uint64_t* out_hashes, int max_count);

#ifdef __cplusplus
}
#endif

#endif
