#pragma once

#include <GL/glew.h>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// 引擎纹理槽位 → OpenGL 纹理单元
// ---------------------------------------------------------------------------
// 参考工程把「引擎槽位」直接当作 GL 纹理单元用：bind(slot) + set_int(name, slot)
// 两边写的是同一个值，因此采样器永远指向真正绑定的那个单元。
//
// 移植工程引入了更多高位槽位（SSAO=33、接触阴影=34、SSR=35、SSIL=37、
// 点光阴影=43/44、GI=45/46 …），而 GL 片段阶段可用纹理单元上限
// （GL_MAX_TEXTURE_IMAGE_UNITS，NVIDIA/Intel 均为 32）低于这些槽位。
// 于是需要把高位槽位压到空闲单元：
//   - 纹理绑定：bind(texture_unit_for_slot(slot))
//   - 采样器 uniform 值：同样必须写 texture_unit_for_slot(slot)
// 两边必须用同一个映射，否则采样器指向的单元上没有纹理，采样恒为 0
//（表现为金属只剩高光、环境光/IBL 被 AO 因子乘没 →「材质像坏了」）。
//
// 映射约束：同一次 draw 会同时绑定的槽位映射后必须互不冲突。
//   PBR draw : 33→2(SSAO) 34→3(接触阴影) 43→4 44→5(点光阴影) 45→6 46→8(GI)
//   SSR 系列 : 35→2(反射) 36→3 37→4 38→5 39→6(HiZ 0..3)
//   DOF/FSR2/运动向量/TAA : 40→8 41→9 32→9（分属不同 pass）

// 查询并缓存 GL_MAX_TEXTURE_IMAGE_UNITS（片段阶段）。无 context 时回退 32。
inline int gl_max_texture_image_units() {
    static int cached = 0;
    if (cached <= 0) {
        GLint units = 0;
        if (glGetIntegerv) {
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        }
        cached = (units > 0) ? static_cast<int>(units) : 32;
    }
    return cached;
}

inline int gl_texture_unit_for_slot(int slot, int max_units) {
    if (slot < 0) return 0;
    if (slot < max_units) return slot;
    switch (slot) {
        case 33: return 2;   // kPBRSSAO
        case 34: return 3;   // kTonemapContactShadow
        case 43: return 4;   // kPointShadow0
        case 44: return 5;   // kPointShadow1
        case 45: return 6;   // kGITexture / kSDFGITexture
        case 46: return 8;   // kVoxelGITexture
        case 35: return 2;   // kSSRTexture
        case 36: return 3;   // kSSRHiZ + 0
        case 37: return 4;   // kSSRHiZ + 1 / kSSILTexture
        case 38: return 5;   // kSSRHiZ + 2 / kDOFHalf
        case 39: return 6;   // kSSRHiZ + 3 / kDOFBlur
        case 40: return 8;   // kFSR2Output
        case 41: return 9;   // kMotionVectors
        case 32: return 9;   // kTAAHistory
        default: return 9;   // 其余高位槽位统一落到 9（当前无同 draw 冲突）
    }
}

} // namespace gryce_engine::render
