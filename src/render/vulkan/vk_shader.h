#pragma once

#include "render/shader.h"
#include "render/skinned_vertex.h"
#include "render/vulkan/vk_buffer.h"

#include <vulkan/vulkan.h>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>
#include <string>

namespace gryce_engine::render {

class VulkanDevice;
class VulkanSwapchain;
class VulkanTexture;

// ---------------------------------------------------------------------------
// VulkanShader — SPIR-V + pipeline layout + descriptor set layout
// ---------------------------------------------------------------------------
class VulkanShader : public IShader {
public:
    VulkanShader() = default;
    VulkanShader(VulkanDevice* device, VulkanSwapchain* swapchain);
    ~VulkanShader() override;

    bool compile(const std::string& vertex_src, const std::string& fragment_src) override;
    bool compile(const std::vector<ShaderStageDesc>& stages) override;

    void bind() const override;
    void unbind() const override;

    void set_int(const std::string& name, int value) override;
    void set_int(const char* name, int value) override;
    void set_float(const std::string& name, float value) override;
    void set_float(const char* name, float value) override;
    void set_vec2(const std::string& name, const math::Vector2f& value) override;
    void set_vec2(const char* name, const math::Vector2f& value) override;
    void set_vec3(const std::string& name, const math::Vector3f& value) override;
    void set_vec3(const char* name, const math::Vector3f& value) override;
    void set_vec4(const std::string& name, const math::Vector4f& value) override;
    void set_vec4(const char* name, const math::Vector4f& value) override;
    void set_mat4(const std::string& name, const math::Matrix4f& value) override;
    void set_mat4(const char* name, const math::Matrix4f& value) override;
    void set_mat4_array(const char* name, const math::Matrix4f* data, uint32_t count) override;
    void set_texture(int slot, ITexture* texture) override;

    bool is_valid() const override;

    VkPipelineLayout layout() const { return pipeline_layout_; }
    VkPipeline pipeline() const { return pipeline_; }
    VkDescriptorSetLayout descriptor_set_layout() const { return descriptor_set_layout_; }
    VkDescriptorSet descriptor_set() const;
    int current_frame() const;

    bool is_post_process() const { return post_process_; }
    bool is_skybox() const { return skybox_; }
    // post-process 与 skybox（及水性材质）共用每帧固定描述符集，
    // 标准 PBR 路径则每 draw 分配独立描述符集。
    bool uses_fixed_descriptor_sets() const { return post_process_ || skybox_ || water_; }

    bool load_program(const std::string& name,
                      const std::string& shader_dir,
                      IFramebuffer* target = nullptr,
                      bool color_output = true,
                      bool post_process = false,
                      bool skybox = false,
                      bool skinned = false) override;

    void set_post_process_params(const PostProcessParams& params) override {
        pp_params_ = params;
        // SSR 的相机近远/半视角/宽高比等字段由管线每帧写入 params，
        // 在此同步进 SSR 专用 push 块。
        if (push_kind_ == PostProcessPushKind::SSR) {
            ssr_push_.near_plane = params.ssr_near;
            ssr_push_.far_plane = params.ssr_far;
            ssr_push_.tan_half_fov = params.ssr_tan_half;
            ssr_push_.aspect = params.ssr_aspect;
            ssr_push_.max_roughness = params.ssr_max_roughness;
            ssr_push_.max_steps = params.ssr_max_steps;
            ssr_push_.thickness = params.ssr_thickness;
            ssr_push_.bilateral_filter = params.ssr_bilateral_filter;
        }
    }

    bool shader_files_changed() const override;
    bool reload() override;

    void update_ubo(VkCommandBuffer cmd) const;
    void push_constants(VkCommandBuffer cmd) const;
    void push_post_process_constants(VkCommandBuffer cmd, const PostProcessParams& params) const;
    void bind_descriptor_set(VkCommandBuffer cmd) const;

    // 每帧开始（渲染线程、acquire 完成之后调用）：重置该帧的描述符池与
    // draw 游标。pool 中上一周期分配的描述符集所属的命令缓冲已被
    // frame fence 保证执行完毕，因此整池 reset 是安全的。
    void on_begin_frame(int frame_index);

    // 每次 draw 调用：把当前 UBO 数据写入该帧大 UBO 的独立偏移，
    // 从该帧描述符池分配一套全新描述符集（UBO + 当前贴图），写入并绑定。
    // 这样同一帧内不同材质的 draw 互不覆盖——共享一套描述符会导致
    // GPU 执行时所有 draw 读到最后一个写入的材质。
    void prepare_draw(VkCommandBuffer cmd);

    // 纹理销毁时由 backend 调用：清除 current_textures_ / cached_textures_ 中
    // 对该指针的缓存。池槽位复用后同一地址可能属于新纹理，裸指针相等会误判
    // "已绑定"而跳过 descriptor 更新；per-draw 路径则会直接用悬垂指针取 image_view。
    void invalidate_texture_cache(const VulkanTexture* tex);

private:
    bool load_spirv_from_file(const std::string& path, std::vector<uint32_t>& out);
    bool load_spirv_files(const std::string& vert_path, const std::string& frag_path);
    void set_render_pass(VkRenderPass render_pass) { render_pass_ = render_pass; }
    void set_color_output_enabled(bool enabled) { color_output_enabled_ = enabled; }
    void set_post_process(bool pp) { post_process_ = pp; }
    void set_skybox(bool skybox) { skybox_ = skybox; }
    bool create_pipeline();
    VkShaderModule create_shader_module(const std::vector<uint32_t>& code);
    bool create_descriptor_pool();
    bool create_ubo();
    // 解析 "uLightPos[3]" 形式的光源数组下标
    static bool parse_light_index(const std::string& name, const char* field, int& index);

    VulkanDevice* device_ = nullptr;
    VulkanSwapchain* swapchain_ = nullptr;

    // Shader 热重载：load_program 记录实际命中的源码文件（磁盘/bundle 解出的
    // 可读路径）与 mtime。优先源码（首编路径）；shaderc 不可用回退到 `.spv` 时
    // 记录 SPIR-V 文件路径。
    std::string source_name_;
    std::string shader_dir_;             // 原始 res: 着色器目录（reload 重解析用）
    std::string spirv_dir_;              // 回退路径：shader_dir/spirv/
    std::string vertex_source_path_;     // 命中顶点源的可读路径
    std::string fragment_source_path_;   // 命中片段源的可读路径
    std::filesystem::file_time_type vert_mtime_{};
    std::filesystem::file_time_type frag_mtime_{};

    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    bool color_output_enabled_ = true;
    bool post_process_ = false;
    bool skybox_ = false;
    // 接触阴影：独立小 push constant 块（48 字节），不占用共享后处理块。
    // 共享块已 224 字节，追加接触阴影字段会超过设备 maxPushConstantsSize(256)。
    bool contact_shadow_ = false;
    // 水性材质（非后处理 mesh pass）：独立固定描述符集 + WaterPushData
    bool water_ = false;
    // 特效后处理 push constant 块的选择（决定 create_pipeline 的 push range 与
    // set_uniform_* 路由）：每个特效独立块避免共享块超 maxPushConstantsSize(256)。
    enum class PostProcessPushKind { General = 0, ContactShadow, SSR, Motion, Fog };
    PostProcessPushKind push_kind_ = PostProcessPushKind::General;
    // 骨骼蒙皮管线：顶点布局追加 bone ids/weights（stride 88），
    // 描述符布局追加 palette UBO（binding 8，vertex stage）
    bool skinned_ = false;

    // 与 GLSL std140 对齐的单光源结构（64 字节，与 GLSL Light 对应）
    struct LightUBO {
        math::Vector4f pos_type;         // xyz=position, w=type (0 方向光/1 点光/2 聚光)
        math::Vector4f dir_range;        // xyz=direction, w=range
        math::Vector4f color_intensity;  // xyz=color, w=intensity
        math::Vector4f spot;             // x=cos(outer), y=cos(inner)
    };
    static constexpr int k_max_lights = 8;
    static constexpr int k_max_cascades = 4;

    // 与 GLSL std140 对齐的 UBO（material + ambient + lights）
    // GLSL 中 vec3 一律以 vec4 存储，避免尾部填充不一致
    struct alignas(16) UBOData {
        math::Vector4f albedo_color;
        math::Vector4f camera_pos;
        math::Vector4f emissive_opacity; // xyz=emissive, w=opacity
        math::Vector4f ambient;          // xyz=环境光颜色
        math::Vector4f uv_transform;     // xy=uv scale, zw=uv offset
        float roughness;
        float metallic;
        float ao;
        int use_shadow_map;
        int use_albedo_map;
        int use_normal_map;
        int use_roughness_map;
        int use_metallic_map;
        int use_ao_map;
        int use_emissive_map;
        int hdr_enabled;
        int light_count;
        int shadow_light_index;
        int use_ibl;
        float ibl_intensity;
        int two_sided;
        // std140：结构体数组必须 16 字节对齐。Vector4f 对齐为 4，two_sided 之后
        // 若不齐pad，lights 会落在 148（GLSL 期望 160）——灯光数据整体错位 12
        // 字节，shader 读到全零（场景无光照）。此前 lights 恰好在 144 只是运气。
        float _pad_std140[4];
        LightUBO lights[k_max_lights];

        // CSM cascaded shadow maps (appended: old offsets unchanged)
        int cascade_count;
        int pcss_enabled;
        int debug_mode;          // 0 final, 1 albedo, 2 normal, 3 roughness,
                                 // 4 metallic, 5 shadow, 6 direct, 7 indirect, 8 cascade
        int _pad_cascade;
        math::Vector4f cascade_splits;         // xyz = view-space split distances
        math::Vector4f cascade_bias;           // xyz = per-cascade depth bias
        math::Vector4f cascade_far_blend;      // x = far, y = cascade blend band ratio
        float pcss_light_size;                 // PCSS light size (world units)
        float pcss_max_radius;                 // PCSS max sample radius (texels)
        float pcss_tap_scale;                  // PCSS tap density scale
        float _pad_cascade2;
        math::Matrix4f cascade_light_space[k_max_cascades];
        math::Matrix4f view_matrix;            // 片段阶段级联深度选择用

        // 材质扩展：Clearcoat / Sheen / 各向异性（追加：旧字段偏移不变）
        float clearcoat;
        float clearcoat_roughness;
        float sheen;
        float anisotropy;
        float anisotropy_rotation;
        float _pad_mat[3];
        math::Vector4f sheen_tint;

        // 屏幕空间环境光遮蔽（GTAO/SSAO 结果）
        int use_ssao;
        float ssao_strength;
        float _pad_ssao[2];
    };
    // layout 必须与 vulkan_pbr.frag / vulkan_skinned_pbr.frag 的 MaterialLightUBO 一致
    // （追加字段会破坏偏移，故不再扩展本结构；特效 pass 参数统一走 PassParamsUBO）
    // 布局必须与 vulkan_pbr.frag / vulkan_skinned_pbr.frag 的 MaterialLightUBO 一致
    static_assert(offsetof(UBOData, lights) == 160, "std140: lights must start at offset 160");
    static_assert(sizeof(LightUBO) == 64, "std140: LightUBO must be 64 bytes");
    static_assert(offsetof(UBOData, cascade_light_space) == 752,
                  "std140: cascade_light_space must start at offset 752");
    static_assert(offsetof(UBOData, sheen_tint) == 1104,
                  "std140: sheen_tint must start at offset 1104");
    static_assert(sizeof(UBOData) == 1136, "std140: UBOData size mismatch (ssao)");

    // ---- 特效 pass 共享参数（阴影/贴花/点光源等）。绑定 binding=20，顶点+片元双阶段。
    // C++ 与各 vulkan_* 着色器的 PassParams 块 std140 逐字段一致。所有偏移 16 对齐。
    struct alignas(16) PassParamsUBO {
        math::Vector4f esm_param;          // +0   x=esm_exponent
        math::Vector4f point_params;       // +16  x=point_light_range, y=paraboloid_face
        math::Vector4f point_light_pos;    // +32  xyz=点光源位置
        math::Vector4f atlas_offset;       // +48  xy=atlas slot offset, zw=slot size
        math::Vector4f screen_size;        // +64  xy=decal 屏幕宽度/高度
        math::Vector4f decal_albedo;       // +80  xyz=贴花反照率, w=不透明度
        math::Matrix4f decal_inv_view_proj;   // +96
        math::Matrix4f decal_world_to_decal;  // +160
    };
    static_assert(sizeof(PassParamsUBO) == 224, "PassParamsUBO must be 224 bytes");

    // 非 post-process 路径：每 draw 独立描述符 + UBO 偏移。
    // 每帧一个描述符池（on_begin_frame 整池 reset）和一个大 UBO
    // （HOST_VISIBLE|COHERENT），按 draw 游标以 ubo_stride_ 对齐切分。
    static constexpr size_t k_pass_params_align = (sizeof(PassParamsUBO) + 255) / 256 * 256;
    // stride 按 256 对齐（>= minUniformBufferOffsetAlignment 常见最大值）。
    static constexpr size_t ubo_stride_ =
        (sizeof(UBOData) + 255) / 256 * 256 + k_pass_params_align;
    static constexpr size_t k_material_block_size = (sizeof(UBOData) + 255) / 256 * 256;
    static constexpr size_t k_pass_block_offset = k_material_block_size; // 与 256 对齐
    static constexpr uint32_t max_draws_per_frame_ = 2048;
    static constexpr int k_max_texture_bindings = 20;

    std::vector<std::unique_ptr<VulkanBuffer>> ubo_buffers_;      // 每帧一个大 UBO
    std::vector<VkDescriptorPool> descriptor_pools_;              // 每帧一个池
    std::vector<VkDescriptorSet> descriptor_sets_;                // post-process：每帧固定集
    std::vector<uint32_t> draw_counts_;                           // 每帧 draw 游标

    // 骨骼 palette（仅 skinned_ 管线）：每帧一个大 UBO，按 draw 游标切分。
    // 与主 UBO 共用同一 cursor，保证一次 draw 的 material 与 palette 对齐。
    static constexpr size_t palette_stride_ = k_max_skinning_bones * sizeof(math::Matrix4f); // 8192
    static constexpr uint32_t max_skinned_draws_per_frame_ = 256;
    std::vector<std::unique_ptr<VulkanBuffer>> palette_buffers_;  // 每帧一个 palette UBO
    // set_mat4_array("uBonePalette") 写入的当前 palette 缓存（渲染线程本地）
    mutable std::array<math::Matrix4f, k_max_skinning_bones> palette_{};
    mutable uint32_t palette_count_ = 0;

    // 当前各 binding 绑定的贴图（set_texture 记录，prepare_draw 写入新集）
    std::array<VulkanTexture*, k_max_texture_bindings> current_textures_{};

    // 1x1 白色回退贴图：prepare_draw 对每个贴图 binding 都必须写入一个
    // 合法 image view + sampler。新分配的描述符集内容是未定义的，若某个
    // binding 留空而 shader（条件分支被编译器提升后）仍采样它，GPU 会读到
    // 垃圾描述符并可能直接挂死（fence 永不 signal，表现为整个窗口卡死）。
    std::unique_ptr<VulkanTexture> fallback_texture_;
    // 1x1 立方体回退：IBL binding 9/10 是 samplerCube，不能用 2D 回退
    std::unique_ptr<VulkanTexture> fallback_cube_;

    // post-process 仍使用每帧固定描述符集（每帧只绑一张贴图，无串扰问题），
    // 沿用按 frame/binding 的更新缓存。
    mutable std::vector<std::array<VulkanTexture*, k_max_texture_bindings>> cached_textures_;

    mutable UBOData ubo_data_{};
    // 特效 pass 参数（阴影/贴花/点光源）：每 draw 上传到 binding 20。
    mutable PassParamsUBO pass_params_{};
    mutable math::Matrix4f model_;
    mutable math::Matrix4f view_;
    mutable math::Matrix4f projection_;
    mutable math::Matrix4f light_space_matrix_;
    mutable float shadow_normal_offset_ = 0.0f;
    mutable bool ubo_dirty_ = true;

    // Post-process parameters (GL uniforms; Vulkan push constants)
    PostProcessParams pp_params_;
    mutable math::Vector2f pp_blur_direction_{}; // vsm_blur 分离高斯方向

    // Must match vulkan_tonemap.frag PushConstants (std430, 128 bytes)
    struct alignas(16) PostProcessPushData {
        float exposure;
        float ev100;
        int mode;
        int dithering;
        float white_point;
        float black_point;
        float contrast;
        float saturation;
        math::Vector4f lift;
        math::Vector4f gamma;
        math::Vector4f gain;
        math::Vector4f shadows;
        math::Vector4f midtones;
        math::Vector4f highlights;
        int bloom_enabled;
        float bloom_threshold;
        float bloom_intensity;
        float film_grain;
        float vignette;
        float chromatic_aberration;
        int use_lut;
        float lut_strength;
        int auto_exposure;
        float ae_target_luminance;
        float ae_min_exposure;
        float ae_max_exposure;
        float ae_speed;
        int taa_enabled;
        float taa_weight;
        int ssao_enabled;
        float ssao_strength;
        float ssao_radius;
        float ssao_near;
        float ssao_far;
        float ssao_tan_half;
        float ssao_aspect;
        // 复用原 8 字节 padding：tonemap 用 push constants 判断是否应用接触阴影
        int cs_enabled;      // offset 216
        float cs_strength;   // offset 220
        math::Vector2f blur_direction; // offset 224（vsm_blur 分离式高斯方向）
        float _pad_pp[2];
    };
    static_assert(sizeof(PostProcessPushData) == 240, "PostProcessPushData must be 240 bytes");

    // 接触阴影专用 push constant（std430，48 字节，< 设备 maxPushConstantsSize）
    struct ContactShadowPushData {
        int enabled;
        float near_plane;
        float far_plane;
        float tan_half_fov;
        float aspect;
        float radius;
        int steps;
        float strength;
        math::Vector4f light_dir_view; // offset 32（16 对齐）
    };
    static_assert(sizeof(ContactShadowPushData) == 48,
                  "ContactShadowPushData must be 48 bytes");

    // SSR 专用 push constant（std430，128 字节，< 256）。对应 vulkan_ssr_trace.frag
    // 的 PushConstants 块。uView 与光照参数由 set_uniform_* / set_post_process_params 路由。
    struct alignas(16) SSRPushData {
        math::Matrix4f view;        // +0   world -> view
        math::Vector3f camera_pos;  // +64
        math::Vector2f screen_size; // +80
        float near_plane;           // +88
        float far_plane;            // +92
        float tan_half_fov;         // +96
        float aspect;               // +100
        float max_roughness;        // +104
        int max_steps;              // +108
        float thickness;            // +112
        float bilateral_filter;     // +116（blur pass 用它；HIZ/trace 忽略）
        math::Vector2f texel_size;  // +120（HIZ pass 用它；其余忽略）
    };
    static_assert(sizeof(SSRPushData) == 128, "SSRPushData must be 128 bytes");

    // Motion Blur 专用 push constant（std430，16 字节）。对应当前 C++ 提供的参数
    // （screen_size + amount）；矩阵重建的旧 GL 路径未在 C++ 侧喂矩阵，保持同等能力。
    struct alignas(16) MotionPushData {
        math::Vector2f screen_size; // +0
        float amount;               // +8
        float _pad[1];              // +12
    };
    static_assert(sizeof(MotionPushData) == 16, "MotionPushData must be 16 bytes");

    // 体积雾专用 push constant（std430，192 字节，< 256）。对应 vulkan_fog.frag /
    // vulkan_fog_apply.frag 的 PushConstants 块。
    struct alignas(16) FogPushData {
        math::Matrix4f inv_view_proj; // +0
        math::Matrix4f view_matrix;   // +64
        math::Vector3f camera_pos;    // +128
        math::Vector3f fog_color;     // +140
        float density;                // +152
        float height;                 // +156
        math::Vector2f fog_range;     // +160（x=near, y=far）
        math::Vector2f screen_size;   // +168
        int slice_count;              // +176
        int slice_index;              // +180
        float _pad[1];                // +184
    };
    static_assert(sizeof(FogPushData) == 192, "FogPushData must be 192 bytes");

    // 水体材质专用 push constant（std430，192 字节，< 256）。对应 vulkan_water 的
    // PushConstants 块。顶点阶段用 view_proj/model，片元阶段用其余参数。
    struct alignas(16) WaterPushData {
        math::Matrix4f view_proj;     // +0
        math::Matrix4f model;         // +64
        math::Vector3f camera_pos;    // +128
        float water_height;           // +140
        float foam_amount;            // +144
        float time;                   // +148
        float wave_amplitude;         // +152
        float wave_frequency;         // +156
        float wave_speed;             // +160
        float wave_steepness;         // +164
        math::Vector4f water_color;   // +176
    };
    static_assert(sizeof(WaterPushData) == 192, "WaterPushData must be 192 bytes");

    // 各特效 push 块实例（由 set_uniform_* / set_post_process_params 填充，
    // push_constants() 按其 push_kind_ 推送）。
    mutable SSRPushData ssr_push_{};
    mutable MotionPushData motion_push_{};
    mutable FogPushData fog_push_{};
    mutable WaterPushData water_push_{};
};

} // namespace gryce_engine::render
