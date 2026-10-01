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
    float ssao_radius = 0.5f;       // 世界单位（米），AO 几何尺度（内部会按深度缩放屏幕步长）
    // 每帧由管线从相机更新（Vulkan push constants 需要）
    float ssao_near = 0.1f;
    float ssao_far = 100.0f;
    float ssao_tan_half = 0.577f;
    float ssao_aspect = 1.777f;

    // 屏幕空间接触阴影（每帧由管线从相机/光源更新；Vulkan push constants 需要）
    int cs_enabled = 0;
    float cs_near = 0.1f;
    float cs_far = 100.0f;
    float cs_tan_half = 0.577f;
    float cs_aspect = 1.777f;
    float cs_radius = 0.5f;
    int cs_steps = 4;
    float cs_strength = 0.6f;
    // 视图空间方向光方向（指向光源），xyz 有效
    math::Vector4f cs_light_dir_view = math::Vector4f(0.0f, 1.0f, 0.0f, 0.0f);

    // SSR（屏幕空间反射，默认关闭）
    int ssr_enabled = 0;
    float ssr_max_roughness = 0.6f;
    int ssr_max_steps = 64;
    // 屏幕空间反射的深度容差下限（世界单位）。这个值直接决定命中点沿射线
    // "越过"表面的距离：掠射角下 0.1 会换算成 ~35 像素的反射错位，
    // 0.01 时约 3 像素（CPU 复核：命中率 90.9% → 99.3%）。
    float ssr_thickness = 0.01f;
    float ssr_bilateral_filter = 0.5f;
    // 未命中时回退到环境/探针反射的强度：
    //   1.0 = 完全替换（命中处用屏幕空间反射替换该像素原本的 IBL 镜面项，
    //         未命中处保留 IBL/探针 —— 屏幕外方向也有反射，且不再双重计能）
    //   0.0 = 旧行为（SSR 叠加在 IBL 之上）
    float ssr_env_fallback = 1.0f;
    // SSR 内部渲染分辨率缩放（0.25~1.0）：光线步进/模糊按 scale×viewport 渲染，
    // 合成仍在全分辨率，用线性采样放大。0.5 大致省 4 倍 SSR 的 GPU 时间。
    float ssr_resolution_scale = 1.0f;
    // SSR 调试视图（0=正常合成；1=原始反射颜色；2=命中覆盖度；3=场景 alpha=IBL 镜面项）
    int ssr_debug_view = 0;

    // PCSS 软阴影 + 级联深度 bias。
    // 注意：这些值必须跟着 PostProcessParams 走到 *前向* 渲染器 —— 之前
    // render_forward_clustered 把 uPCSSEnabled 写死成 0、级联 bias 写死成
    // 一组常量，于是 API 里设的 pcss_enabled / set_cascade_biases 在 demo
    // （前向路径）上完全不生效，阴影永远是硬边。
    int pcss_enabled = 0;
    float pcss_light_size = 0.05f;   // 光源角半径（决定半影宽度）
    float pcss_max_radius = 16.0f;   // 半影最大半径（阴影贴图 texel 数）
    float pcss_blocker_scale = 1.0f;
    // 每级联的基础深度 bias（[0,1] 归一化深度），xyz = 前三级，w = 第四级。
    // 值必须足够大才能盖过根部自遮挡（shadow acne / Peter-Panning 亮缝）。
    // 之前误降到 {0.0001,...}，正面 bias 被压到 0.000005，根本不起作用，
    // 根部亮缝直接复发。恢复到合理量级：正面 bias 由 slope_bias 下限控制，
    // 深度 bias 本身负责消除 shadow acne，Peter-Panning 交给 normal-offset。
    math::Vector4f cascade_bias = math::Vector4f(0.001f, 0.002f, 0.004f, 0.008f);
    // 每帧由管线从相机更新（Vulkan push constants 需要）
    float ssr_near = 0.1f;
    float ssr_far = 100.0f;
    float ssr_tan_half = 0.577f;
    float ssr_aspect = 1.777f;

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

    // Motion Blur（运动模糊，默认关闭）
    int motion_blur_enabled = 0;
    float motion_blur_amount = 0.5f;  // 模糊强度 0~1

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

    // 该 shader 的图形管线是否需要 alpha 混合。
    // OpenGL 由 render context 的 set_blend 命令逐 pass 控制，这里忽略即可；
    // Vulkan 的混合状态在创建管线时定死，必须显式声明：
    //   - 普通材质：true（半透明材质需要 alpha blend，不透明材质 alpha=1 结果不变）
    //   - 数据通道（深度+法线预通道 / G-buffer / 阴影）：false
    //     —— 否则写出的数据会和清屏色混合（预通道的 alpha 是粗糙度！），
    //        法线被污染 → SSR 反射方向全错。
    virtual void set_pipeline_blending(bool enabled) { (void)enabled; }

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
