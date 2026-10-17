#pragma once

#include <GL/glew.h>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// 引擎纹理槽位 → OpenGL 纹理单元
// ---------------------------------------------------------------------------
// 参考工程把「引擎槽位」直接当作 GL 纹理单元用：bind(slot) + set_int(name, slot)
// 两边写的是同一个值，因此采样器永远指向真正绑定的那个单元。
//
// 移植工程引入了更多高位槽位（SSAO=33、接触阴影=34、SSR=35、点光阴影=43/44、
// GI=45/46、VSM=45..48 …），而 GL 片段阶段可用纹理单元上限
// （GL_MAX_TEXTURE_IMAGE_UNITS）分两档：
//   - 桌面 GL（NVIDIA/Intel/AMD）：32
//   - **浏览器 WebGL2：16**（Chrome/ANGLE 固定按 16 校验；SwiftShader 报 32，
//     所以软件光栅下永远测不出这一档的问题）
// 于是需要把高位槽位压到空闲单元：
//   - 纹理绑定：bind(texture_unit_for_slot(slot))
//   - 采样器 uniform 值：同样必须写 texture_unit_for_slot(slot)
// 两边必须用同一个映射，否则采样器指向的单元上没有纹理，采样恒为 0
//（表现为金属只剩高光、环境光/IBL 被 AO 因子乘没 →「材质像坏了」）。
//
// 映射约束（**同一次 draw 会同时绑定的槽位映射后必须互不冲突**，而且
// WebGL 还额外要求同一个单元上不能同时是 sampler2D 和 samplerCube）：
//
//   32 单元（桌面）：
//     PBR draw : 33→2(SSAO) 34→3(接触阴影) 43→4 44→5(点光阴影) 45→6 46→8(GI)
//     SSR 系列 : 35→2(反射) 36→3 37→4 38→5 39→6(HiZ 0..3)
//     DOF/FSR2/运动向量/TAA : 40→8 41→9 32→9（分属不同 pass）
//
//   16 单元（WebGL）：
//     PBR draw : 材料 10..15 原地，emissive(16)→0，级联 1..3(17..19)→1..3，
//                IBL 22..24→4..6，SSAO(33)→7           —— 共 14 个，够用
//     后处理   : 20→0 29→1 30→2 31→3 34→4 32→5 40→6 41→7（与前向不共存）
//     其它独立 : 21→8(天空盒) 47→9(粒子) 45/46→10/11(GI) 35..38→12..15(SSR)
//     WebGL 着色器已裁掉的采样器（PCSS 原始深度 25..28、点光阴影 43/44、
//     VSM 48）→ 死区 16..22：绑上去合法（< GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS），
//     且没有任何采样器引用，绝不会覆盖 0..15 里真实在用的纹理。

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

// 16 单元（WebGL）专用映射。见文件头注释的分配表。
inline int gl_texture_unit_for_slot_16(int slot) {
    switch (slot) {
        // ---- 前向 PBR 主 pass（同一次 draw 共存，必须互不冲突）----
        case 10: return 10;  // kPBRAlbedo
        case 11: return 11;  // kPBRNormal
        case 12: return 12;  // kPBRRoughness
        case 13: return 13;  // kPBRMetallic
        case 14: return 14;  // kPBRAO
        case 15: return 15;  // kPBRShadow（级联 0）
        case 16: return 0;   // kPBREmissive
        case 17: return 1;   // kPBRShadowC1
        case 18: return 2;   // kPBRShadowC2
        case 19: return 3;   // kPBRShadowC3
        case 22: return 4;   // kIBLIrradiance（samplerCube）
        case 23: return 5;   // kIBLPrefilter（samplerCube）
        case 24: return 6;   // kIBLBRDF
        case 33: return 7;   // kPBRSSAO

        // ---- 后处理 / 合成（与前向主 pass 不共存，可复用 0..7）----
        case 20: return 0;   // kTonemapHDR
        case 29: return 1;   // kTonemapBloom
        case 30: return 2;   // kTonemapLUT（3D 纹理）
        case 31: return 3;   // kTonemapExposure
        case 34: return 4;   // kTonemapContactShadow
        case 32: return 5;   // kTAAHistory
        case 40: return 6;   // kFSR2Output
        case 41: return 7;   // kMotionVectors / decal 深度

        // ---- 各自独立的 pass ----
        case 21: return 8;   // kSkyboxCube（samplerCube，天空盒 pass 单独一张）
        case 47: return 9;   // kParticleTexture（粒子 pass；VSM3 也用 47，但 WebGL 已裁）
        case 45: return 10;  // kGITexture（WebGL 着色器已裁，留位给未来）
        case 46: return 11;  // kVoxelGITexture / VSM1
        case 35: return 12;  // kSSRTexture
        case 36: return 13;  // kSSRHiZ + 0
        case 37: return 14;  // kSSRHiZ + 1 / kSSILTexture
        case 38: return 15;  // kSSRHiZ + 2 / kDOFHalf
        case 39: return 8;   // kSSRHiZ + 3 / kDOFBlur（另一个 pass，复用 8）
        case 42: return 9;   // kSSRProbeAtlas（另一个 pass，复用 9）

        // ---- 死区：WebGL 着色器已经把这几组采样器 #ifdef 裁掉，
        //      只保留绑定（< 32 的组合上限），任何采样器都不会引用。----
        case 25: return 16;  // kPBRShadowDepth  （PCSS，WebGL 无）
        case 26: return 17;  // kPBRShadowDepth1
        case 27: return 18;  // kPBRShadowDepth2
        case 28: return 19;  // kPBRShadowDepth3
        case 43: return 20;  // kPointShadow0   （WebGL 无）
        case 44: return 21;  // kPointShadow1
        case 48: return 22;  // VSM 级联 3      （WebGL 无）

        default: break;
    }
    if (slot >= 0 && slot < 10) return slot;   // 2D 渲染器等低槽位：保持原值
    return 23;                                 // 未知高位槽位：落到死区
}

inline int gl_texture_unit_for_slot(int slot, int max_units) {
    if (slot < 0) return 0;
    // WebGL2 真实 GPU 只给 16 个片段纹理单元，用紧凑映射（见文件头）。
    if (max_units <= 16) return gl_texture_unit_for_slot_16(slot);
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
