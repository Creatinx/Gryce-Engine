#ifndef GRYCE_ANIMATOR_API_H
#define GRYCE_ANIMATOR_API_H

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

// 查询 SkinnedMeshRenderer 模型自带的动画片段数量（-1 表示实体/组件无效）。
GRYCE_CORE_API int GAnimator_GetClipCount(GEntityHandle entity, uint64_t comp_type_hash);

// 取第 index 个动画片段名（返回写入长度；-1 失败）。
GRYCE_CORE_API int GAnimator_GetClipName(GEntityHandle entity, uint64_t comp_type_hash,
                                         int index, char* out_buf, int buf_size);

// 取第 index 个动画片段时长（秒；-1 失败）。
GRYCE_CORE_API float GAnimator_GetClipDuration(GEntityHandle entity, uint64_t comp_type_hash,
                                               int index);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_ANIMATOR_API_H
