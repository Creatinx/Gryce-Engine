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

// 查询模型骨架的骨骼数量（-1 表示实体/组件无效）。
GRYCE_CORE_API int GAnimator_GetBoneCount(GEntityHandle entity, uint64_t comp_type_hash);

// 取第 bone_index 根骨骼名（返回写入长度；-1 失败）。
GRYCE_CORE_API int GAnimator_GetBoneName(GEntityHandle entity, uint64_t comp_type_hash,
                                         int bone_index, char* out_buf, int buf_size);

// 取骨骼的父级下标（-1 = 根骨骼；-2 失败）。
GRYCE_CORE_API int GAnimator_GetBoneParentIndex(GEntityHandle entity, uint64_t comp_type_hash,
                                                int bone_index);

// 取第 clip_index 个片段中的骨骼轨道数量（-1 失败）。
GRYCE_CORE_API int GAnimator_GetClipTrackCount(GEntityHandle entity, uint64_t comp_type_hash,
                                               int clip_index);

// 取轨道对应的骨骼下标（-1 失败）。
GRYCE_CORE_API int GAnimator_GetClipTrackBone(GEntityHandle entity, uint64_t comp_type_hash,
                                              int clip_index, int track_index);

// 取轨道指定通道（0=平移, 1=旋转, 2=缩放）的关键帧数量（-1 失败）。
GRYCE_CORE_API int GAnimator_GetClipKeyframeCount(GEntityHandle entity, uint64_t comp_type_hash,
                                                  int clip_index, int track_index, int channel);

// 取关键帧数据：out[0]=time；平移/缩放 out[1..3]=XYZ；旋转 out[1..4]=XYZW。
// 返回写入的 float 数量（4 或 5）；失败返回 -1。
GRYCE_CORE_API int GAnimator_GetClipKeyframe(GEntityHandle entity, uint64_t comp_type_hash,
                                             int clip_index, int track_index, int channel,
                                             int key_index, float* out, int out_capacity);

#ifdef __cplusplus
}
#endif

#endif // GRYCE_ANIMATOR_API_H
