#ifndef GRYCE_EDIT_API_H
#define GRYCE_EDIT_API_H

// ---------------------------------------------------------------------------
// 编辑命令面（编辑器与引擎之间唯一的写入通道）
//
// 引擎的编辑操作全部走 `GCore_PushCommand` 的命令缓冲：命令在本帧边界
// （GCore_BeginFrame）统一消费，从而把"UI 线程的编辑意图"与"场景状态"解耦，
// 也让撤销/重做、Play 快照、批量编辑只需要在一个地方拦截。
//
// 本文件做两件事：
//   1) 公开每条命令的 payload 布局。此前这些结构体只写在 core_api.cpp 的
//      process_command 里，编辑器只能照抄一份 —— 引擎侧一改字段就静默读错字节。
//   2) 提供按语义命名的封装，调用方不必自己拼 256 字节的 payload。
//
// 约定：
//   * 所有函数把命令压入队列后立即返回，返回值只表示"是否入队成功"，
//     不代表命令执行结果。执行期的失败写日志，不回调。
//   * 字符串字段一律为定长数组，超长截断并保证 '\0' 结尾。
// ---------------------------------------------------------------------------

#include "api/types.h"

#ifdef _WIN32
    #ifdef GRYCE_CORE_BUILDING
        #define GRYCE_EDIT_API __declspec(dllexport)
    #else
        #define GRYCE_EDIT_API __declspec(dllimport)
        #ifdef _MSC_VER
            #ifdef _DEBUG
                #pragma comment(lib, "GryceCored.lib")
            #else
                #pragma comment(lib, "GryceCore.lib")
            #endif
        #endif
    #endif
#else
    #define GRYCE_EDIT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 命令 payload 布局（与 GCommand.payload 二进制兼容）
// ---------------------------------------------------------------------------

// ECMD_CREATE_ENTITY
typedef struct {
    char          name[128];
    GEntityHandle parent;      // 0 = 场景根
} GCmdCreateEntity;

// ECMD_RENAME_ENTITY
typedef struct {
    GEntityHandle handle;
    char          name[128];
} GCmdRenameEntity;

// ECMD_REPARENT_ENTITY
typedef struct {
    GEntityHandle handle;
    GEntityHandle parent;      // 0 = 场景根
} GCmdReparentEntity;

// ECMD_SET_TRANSFORM（局部空间 TRS）
typedef struct {
    GEntityHandle handle;
    GVec3         position;
    GQuat         rotation;
    GVec3         scale;
} GCmdSetTransform;

// ECMD_TRANSLATE_ENTITY（局部空间增量平移）
//
// 为什么不复用 GCmdSetTransform：那个是**绝对值**，而且缺省分量在入队时就用
// "当前值"补齐。同一帧里先 setPosition 再 translate 时，第二条命令入队读到的
// 还是旧位置，算出来的绝对值会把第一条覆盖掉（最终只生效一次）。
// 增量命令在**执行时**才读当前值，同帧多条可以正确叠加。
typedef struct {
    GEntityHandle handle;
    GVec3         delta;
} GCmdTranslateEntity;

// ECMD_SET_PROPERTY：value 按反射字段的原始字节写入（float/int/bool/向量），
// 字符串字段写入以 '\0' 结尾的字节序列。
typedef struct {
    GEntityHandle handle;
    uint64_t      type_hash;   // 组件类型名（全名）的 std::hash
    char          prop_name[64];
    uint8_t       value[128];
} GCmdSetProperty;

// ECMD_ADD_COMPONENT
typedef struct {
    GEntityHandle handle;
    char          type_name[128];
} GCmdAddComponent;

// ECMD_REMOVE_COMPONENT
typedef struct {
    GEntityHandle handle;
    uint64_t      type_hash;
} GCmdRemoveComponent;

// ECMD_SET_MATERIAL：给实体（无则加）MeshRenderer 并设置网格与 PBR 参数。
typedef struct {
    GEntityHandle handle;
    char          mesh_path[128];   // 空 = 只改材质，要求已有 MeshRenderer
    float         albedo[3];
    float         roughness;
    float         metallic;
} GCmdSetMaterial;

// ECMD_SET_RENDER_TARGET：切换离屏目标尺寸。target: 0 = 场景视口，1 = 游戏视口。
typedef struct {
    int target;
    int width;
    int height;
} GCmdSetRenderTarget;

// ECMD_GIZMO_MANIPULATE：把一次拖拽增量应用到选中实体。
// delta 的语义随 gizmo 操作与空间变化（见 GRender_SetGizmoOperation）。
typedef struct {
    int   operation;   // 0 = 平移，1 = 旋转，2 = 缩放
    int   space;       // 0 = 世界，1 = 局部
    float delta[3];    // 平移(世界单位) / 旋转(弧度, XYZ) / 缩放(增量)
} GCmdGizmoManipulate;

// 编译期守卫：payload 超过命令槽位的布局在入队时就会被静默截断，
// 这类错误必须在编译期暴露，不能等到运行期读错字节。
#ifdef __cplusplus
static_assert(sizeof(GCmdCreateEntity)    <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdRenameEntity)    <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdReparentEntity)  <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdSetTransform)    <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdTranslateEntity) <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdSetProperty)     <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdAddComponent)    <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdRemoveComponent) <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdSetMaterial)     <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdSetRenderTarget) <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
static_assert(sizeof(GCmdGizmoManipulate) <= 256, "payload exceeds GCMD_PAYLOAD_SIZE");
#endif

// ---------------------------------------------------------------------------
// 层级与变换
// ---------------------------------------------------------------------------

// 创建实体并挂到 parent 下（0 = 场景根）。返回 1 表示已入队。
GRYCE_EDIT_API int GEdit_CreateEntity(const char* name, GEntityHandle parent);
GRYCE_EDIT_API int GEdit_DestroyEntity(GEntityHandle entity);
GRYCE_EDIT_API int GEdit_RenameEntity(GEntityHandle entity, const char* name);
GRYCE_EDIT_API int GEdit_ReparentEntity(GEntityHandle entity, GEntityHandle parent);
GRYCE_EDIT_API int GEdit_SelectEntity(GEntityHandle entity);

// 一次性写入局部 TRS（Gizmo 拖拽、Inspector 数值框、撤销恢复都走这条）。
// rotation 传 0,0,0,0 表示保持原旋转不变。
GRYCE_EDIT_API int GEdit_SetTransform(GEntityHandle entity,
                                      const GVec3* position,
                                      const GQuat* rotation,
                                      const GVec3* scale);

// 局部空间增量平移。执行时才读当前位置，因此同一帧内连续调用可以叠加；
// 需要在同一帧里"先设绝对位置再微调"的场合必须用这条，不能两次 SetTransform。
GRYCE_EDIT_API int GEdit_TranslateEntity(GEntityHandle entity, const GVec3* delta);

// ---------------------------------------------------------------------------
// 组件属性
// ---------------------------------------------------------------------------

// 按反射字段写属性。value/value_size 是字段原始字节（字符串传 C 字符串）。
GRYCE_EDIT_API int GEdit_SetProperty(GEntityHandle entity, uint64_t type_hash,
                                     const char* prop_name,
                                     const void* value, int value_size);
GRYCE_EDIT_API int GEdit_AddComponent(GEntityHandle entity, const char* type_name);
GRYCE_EDIT_API int GEdit_RemoveComponent(GEntityHandle entity, uint64_t type_hash);
GRYCE_EDIT_API int GEdit_SetMaterial(GEntityHandle entity, const char* mesh_path,
                                     float r, float g, float b,
                                     float roughness, float metallic);

// ---------------------------------------------------------------------------
// 场景与运行控制
// ---------------------------------------------------------------------------

// 请求存档。0 = 场景视口，1 = 游戏视口。
GRYCE_EDIT_API int GEdit_SetRenderTargetSize(int target, int width, int height);
GRYCE_EDIT_API int GEdit_SaveScene(const char* path);
GRYCE_EDIT_API int GEdit_ImportAsset(const char* path);

// 0 = 回到编辑态（Stop），1 = 进入 Play，2 = 切换暂停。
GRYCE_EDIT_API int GEdit_SetPlayMode(int mode);

// 单帧步进：Play 且暂停时前进 frames 帧。引擎在消费该命令后自动恢复暂停。
GRYCE_EDIT_API int GEdit_StepFrame(int frames);

#ifdef __cplusplus
}
#endif

#endif  // GRYCE_EDIT_API_H
