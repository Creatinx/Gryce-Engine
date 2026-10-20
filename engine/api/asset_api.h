#ifndef GRYCE_ASSET_API_H
#define GRYCE_ASSET_API_H

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

GRYCE_CORE_API GAssetHandle GAsset_Import(const char* source_path);
GRYCE_CORE_API GAssetHandle GAsset_Load(const char* path);
GRYCE_CORE_API int          GAsset_GetPath(GAssetHandle handle, char* out_buf, int buf_size);
GRYCE_CORE_API void         GAsset_Unload(GAssetHandle handle);

// ---------------------------------------------------------------------------
// 资产侧车文件（<asset>.meta）
//
// 编辑器需要"资产的身份"独立于它的路径：层级里的引用、材质槽、场景片段都按
// guid 指向资产，重命名/移动文件不会断链。侧车还保存导入设置（缩放、上轴、
// 材质开关、粗糙度倍率），下次导入按它生效。
//
// 侧车缺失时 GAsset_EnsureMeta 会按源文件生成一份并写盘（目录只读时只返回
// 内存中的结果），因此调用方不需要额外的"先创建"步骤。
// ---------------------------------------------------------------------------
typedef struct {
    char  guid[40];            // 32 位十六进制 + 结束符
    char  source_path[512];    // 源资产路径（诊断用）
    char  importer[32];        // "mesh" / "texture" / "skinned" / "audio"
    float scale;               // 源单位 → 米
    int   up_axis;             // 0=X-up，1=Y-up，2=Z-up
    int   generate_lod;        // 1 = 导入后生成 LOD 链
    int   import_materials;    // 1 = 采用文件自带材质
    float roughness_scale;     // 叠在推导结果上的粗糙度倍率
    int   version;             // 侧车格式版本
} GAssetMetaInfo;

// 读侧车（不存在或损坏返回 0，不改动 out）。
GRYCE_CORE_API int  GAsset_ReadMeta(const char* asset_path, GAssetMetaInfo* out);
// 读侧车；不存在则生成（含新 guid 与源文件指纹）并写盘。
GRYCE_CORE_API int  GAsset_EnsureMeta(const char* asset_path, GAssetMetaInfo* out);
// 写侧车（缺失字段用当前引擎默认值补齐）。返回 1/0。
GRYCE_CORE_API int  GAsset_WriteMeta(const char* asset_path, const GAssetMetaInfo* info);
// 源文件指纹与侧车记录不一致时为 1 —— 编辑器据此提示"资源已变更，是否重新导入"。
GRYCE_CORE_API int  GAsset_IsMetaStale(const char* asset_path);

// ---------------------------------------------------------------------------
// 目录列举
//
// 资产浏览器、打包清单、热更校验都需要"某个目录底下有哪些资产"，逐个路径去
// 试 GAsset_Load 既慢又区分不出"文件不存在"和"格式不支持"。本接口只做文件
// 系统遍历，不加载任何数据。
//
// dir 接受 res:/ 形式（按项目根解析）或真实路径。recursive 非 0 时递归子目录。
// 输出 JSON（UTF-8，单行）：
//   {"dir":"res:/models","count":2,"truncated":0,"items":[
//     {"path":"res:/models/cube.obj","name":"cube.obj","ext":"obj",
//      "size":1234,"guid":"...","has_meta":1,"stale":0,"kind":"mesh"}]}
//
// 返回值：写入 out_buf 的字节数（不含结尾 0）。缓冲不足时返回**所需**长度
// （大于 buf_size，调用方据此扩容重试），失败返回 -1。
// kind 取值：mesh / texture / audio / scene / script / shader / other，
// 由扩展名推断，只用于列表分组，不代表能否成功导入。
// 结果按 path 字典序排列，保证同一目录每次列举顺序一致（界面不抖动）。
// ---------------------------------------------------------------------------
GRYCE_CORE_API int GAsset_ScanDirectory(const char* dir, int recursive,
                                        char* out_buf, int buf_size);

// 访客式遍历：同样的目录、同样的顺序（path 字典序）、同样的排除规则，
// 但把每个条目直接交给回调，不经过 JSON。
//
// 为什么两种都有：JSON 版是给脚本 / 外部宿主用的（跨进程、跨语言都方便），
// 而 UI 自己画列表时再去解析自己刚生成的 JSON 是纯浪费 —— 资产几千条时
// 每秒解析一遍明显掉帧，而且解析失败的表现是"列表空白"，很难查。
//
// 回调里拿到的指针只在本次调用期间有效（指向内部临时对象），需要留就自己拷。
// 回调返回 0 表示中止遍历（例如列表已经填满、用户已经切目录）。
// 返回值：访问到的条目数；目录不存在返回 -1。
typedef struct GAssetEntry {
    const char*    path;     // res:/ 形式（不在项目根下时为绝对路径）
    const char*    name;     // 文件名含扩展名
    const char*    ext;      // 小写，含点（".obj"）
    const char*    kind;     // mesh / texture / audio / scene / script / shader / other
    const char*    guid;     // 侧车里的 guid，没有侧车时为空串
    unsigned long long size; // 字节数，取不到时为 0
    int            has_meta;
    int            stale;
} GAssetEntry;

typedef int (*GAssetVisitor)(void* user, const GAssetEntry* entry);

GRYCE_CORE_API int GAsset_ScanDirectoryEach(const char* dir, int recursive,
                                            GAssetVisitor visitor, void* user);

#ifdef __cplusplus
}
#endif

#endif
