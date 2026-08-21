#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "math/math.h"
#include "render/texture.h"

namespace gryce_engine::render {

class IFramebuffer;

// ---------------------------------------------------------------------------
// ShaderStage — Shader 阶段（支持扩展 Geometry/Compute）
// ---------------------------------------------------------------------------
enum class ShaderStage {
    Vertex,
    Fragment,
    Geometry,
    Compute
};

// ---------------------------------------------------------------------------
// ShaderStageDesc — Shader 阶段描述
// ---------------------------------------------------------------------------
struct ShaderStageDesc {
    ShaderStage stage = ShaderStage::Vertex;
    std::string source;
    std::string entry_point = "main";

    ShaderStageDesc() = default;
    ShaderStageDesc(ShaderStage s, std::string src, std::string entry = "main")
        : stage(s), source(std::move(src)), entry_point(std::move(entry)) {}
};

// ---------------------------------------------------------------------------
// PostProcessParams — post-process (tonemap) 参数
// 由 RenderPipeline 设置；GL 后端写成 uniform，Vulkan 后端走 push constants。
// ---------------------------------------------------------------------------
struct PostProcessParams {
    float exposure = 1.0f;
    float ev100 = -1.0f;      // >= 0 时按摄影 EV100 推导曝光
    int tone_map_mode = 1;    // 0 none, 1 Reinhard, 2 ACES, 3 AgX, 4 filmic
    int dithering = 1;        // 8-bit 输出前有序抖动

    float white_point = 1.0f;
    float black_point = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;

    // Bloom 后处理（阈值提取 → 多级降采样模糊 → 上采样合成）
    int bloom_enabled = 1;
    float bloom_threshold = 1.0f;
    float bloom_intensity = 0.35f;

    // 轻量镜头效果（默认关闭）
    float film_grain = 0.0f;          // 0~1
    float vignette = 0.0f;            // 0~1
    float chromatic_aberration = 0.0f; // 0~1

    // 3D LUT 色彩分级（配合 set_color_lut 的 1024x32 打包贴图）
    int use_lut = 0;
    float lut_strength = 1.0f;

    // 自动曝光（GPU 侧亮度反馈，默认关闭）
    int auto_exposure = 0;
    float ae_target_luminance = 0.18f;
    float ae_min_exposure = 0.1f;
    float ae_max_exposure = 4.0f;
    float ae_speed = 1.0f;

    // TAA（时域累积 + 抖动采样 + 邻域钳制，默认关闭）
    int taa_enabled = 0;
    float taa_weight = 0.85f;

    // 屏幕空间环境光遮蔽（GTAO-lite + 双边上模糊，默认关闭）
    int ssao_enabled = 0;
    float ssao_strength = 1.0f;
    float ssao_radius = 12.0f;      // 屏幕空间采样半径（像素）
    // 每帧由管线从相机更新（Vulkan push constants 需要）
    float ssao_near = 0.1f;
    float ssao_far = 100.0f;
    float ssao_tan_half = 0.577f;
    float ssao_aspect = 1.777f;

    // SSR（屏幕空间反射，默认关闭）
    int ssr_enabled = 0;
    float ssr_max_roughness = 0.6f;
    int ssr_max_steps = 64;
    float ssr_thickness = 0.1f;
    float ssr_bilateral_filter = 0.5f;

    // SSIL（屏幕空间间接光照，默认关闭）
    int ssil_enabled = 0;
    float ssil_strength = 0.3f;
    float ssil_radius = 2.0f;

    // Bokeh DOF（景深，默认关闭）
    int dof_enabled = 0;
    float dof_focus_distance = 10.0f;
    float dof_focus_radius = 5.0f;   // 聚焦范围（距离两侧）
    float dof_blur_amount = 3.0f;
    float dof_max_coc = 20.0f;       // 最大弥散圆半径（像素）

    // FSR2（超分辨率，默认关闭）
    int fsr2_enabled = 0;
    float fsr2_sharpness = 0.5f;
    int fsr2_render_width = 0;
    int fsr2_render_height = 0;

    // SSS（次表面散射，默认关闭）
    int sss_enabled = 0;
    float sss_strength = 1.0f;
    float sss_scale = 10.0f;

    math::Vector4f lift = math::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
    math::Vector4f gamma = math::Vector4f(1.0f, 1.0f, 1.0f, 0.0f);
    math::Vector4f gain = math::Vector4f(1.0f, 1.0f, 1.0f, 0.0f);
    math::Vector4f shadows = math::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
    math::Vector4f midtones = math::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
    math::Vector4f highlights = math::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);
};

// ---------------------------------------------------------------------------
// IShader — 跨 API Shader 接口
// ---------------------------------------------------------------------------
class IShader {
public:
    virtual ~IShader() = default;

    // 便捷接口：编译顶点 + 片段着色器（旧代码兼容）
    virtual bool compile(const std::string& vertex_src,
                         const std::string& fragment_src) = 0;

    // 通用接口：按阶段编译（支持多阶段、指定入口）
    virtual bool compile(const std::vector<ShaderStageDesc>& stages) = 0;

    virtual void bind() const = 0;
    virtual void unbind() const = 0;

    virtual void set_int(const std::string& name, int value) = 0;
    virtual void set_int(const char* name, int value) = 0;
    virtual void set_float(const std::string& name, float value) = 0;
    virtual void set_float(const char* name, float value) = 0;
    virtual void set_vec2(const std::string& name, const gryce_engine::math::Vector2f& value) = 0;
    virtual void set_vec2(const char* name, const gryce_engine::math::Vector2f& value) = 0;
    virtual void set_vec3(const std::string& name, const gryce_engine::math::Vector3f& value) = 0;
    virtual void set_vec3(const char* name, const gryce_engine::math::Vector3f& value) = 0;
    virtual void set_vec4(const std::string& name, const gryce_engine::math::Vector4f& value) = 0;
    virtual void set_vec4(const char* name, const gryce_engine::math::Vector4f& value) = 0;
    virtual void set_mat4(const std::string& name, const gryce_engine::math::Matrix4f& value) = 0;
    virtual void set_mat4(const char* name, const gryce_engine::math::Matrix4f& value) = 0;

    // mat4 数组 uniform（骨骼 palette 等）。count 超上限时实现侧截断并告警。
    // 默认 no-op：不支持数组 uniform 的后端/测试 mock 不受影响。
    virtual void set_mat4_array(const char* /*name*/, const gryce_engine::math::Matrix4f* /*data*/,
                                uint32_t /*count*/) {}

    // 后端相关纹理绑定（OpenGL 可忽略，Vulkan 用于更新 descriptor set）
    virtual void set_texture(int slot, ITexture* texture) { (void)slot; (void)texture; }

    // Load a shader program by base name from a shader directory.
    // OpenGL backend loads `{dir}/{name}.vert` and `{dir}/{name}.frag` as GLSL source.
    // Vulkan backend loads `{dir}/spirv/vulkan_{name}.vert.spv` and `{dir}/spirv/vulkan_{name}.frag.spv`.
    // target/color_output/post_process are used by Vulkan to build the pipeline.
    // skybox=true 时（Vulkan）构建天空盒管线：单 cubemap sampler、深度 LESS_OR_EQUAL、不写深度、不剔除。
    // skinned=true 时（Vulkan）构建骨骼蒙皮管线：顶点布局追加 bone ids/weights，
    // 描述符布局追加 palette UBO（binding 8，vertex stage）。
    virtual bool load_program(const std::string& name,
                              const std::string& shader_dir,
                              IFramebuffer* target = nullptr,
                              bool color_output = true,
                              bool post_process = false,
                              bool skybox = false,
                              bool skinned = false) { (void)skybox; (void)skinned; return false; }

    // Set post-process (tonemap) parameters. Used by tonemap shader.
    virtual void set_post_process_params(const PostProcessParams& params) {
        (void)params;
    }

    virtual bool is_valid() const = 0;

    // -----------------------------------------------------------------------
    // Shader 热重载
    // -----------------------------------------------------------------------
    // 检测着色器源文件（OpenGL 为 .vert/.frag，Vulkan 为 SPIR-V）是否已变化。
    // 仅做文件 stat，主线程调用安全（不需要 GL/VK context）。
    virtual bool shader_files_changed() const { return false; }

    // 重新读取源文件并重新编译 / 重建管线。调用前必须 pause_render_thread()，
    // 且调用方持有 GPU context。编译失败时保留旧程序/管线，返回 false。
    virtual bool reload() { return false; }
};

} // namespace gryce_engine::render
