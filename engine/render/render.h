// render.h — 渲染后端抽象层（RHI）：IRenderBackend 接口与后端能力查询。
// OpenGL / Vulkan / D3D12 三后端实现同一接口，上层（RenderContext /
// RenderPipeline）只面向此接口编程；逐帧状态一律经 RenderContext 命令缓冲
// 提交，主线程不得直接调用后端的变值接口。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "render/export.h"
#include "render/render2d.h"
#include "render/imgui_backend.h"
#include "render/rhi_handle.h"
#include "render/render_commands.h"

namespace gryce_engine::render {

// 前向声明
class IMesh;
class IShader;
class ITexture;
class IFramebuffer;
class IBuffer;

// ---------------------------------------------------------------------------
// RenderBackendCapabilities — 后端能力查询
// 将 vendor-specific 特性、纹理格式支持、动态状态等显式化，避免 base 接口泄漏。
// ---------------------------------------------------------------------------
struct RenderBackendCapabilities {
    // 通用能力
    bool supports_compute = false;
    bool supports_geometry_shader = false;
    bool supports_dynamic_state = false;        // Vulkan extended dynamic state 等
    uint32_t max_texture_slots = 16;
    uint32_t max_push_constant_size = 128;

    // 帧率/呈现相关
    bool supports_vsync_control = false;        // swap interval / present mode
    bool supports_gpu_busy_spin = false;        // 主动占用 GPU 的 pacing 技巧
    bool supports_nv_delay_before_swap = false; // NVIDIA WGL_NV_delay_before_swap

    // 纹理格式
    bool supports_srgb = true;
    bool supports_depth32f = true;
    bool supports_r8 = true;
    bool supports_rgba16f = false;
    // 多样本颜色附件支持的最大采样数（1 = 不支持 MSAA；按 2 的幂上报）。
    uint32_t max_msaa_color_samples = 1;
    // MSAA 目标的深度附件是否必须与颜色同采样数：
    //   OpenGL 规范要求 FBO 内所有附件采样数一致 → true（深度也建多样本，
    //   resolve 时与颜色一起 blit 回单样本）；
    //   Vulkan / D3D12 允许颜色多样本 + 深度单样本混合 → false（深度直接
    //   共用单样本附件，省一份多样本深度显存，也无需深度 resolve）。
    bool msaa_depth_must_match_samples = false;

    // 硬件光追（Vulkan ray query / D3D12 DXR）：仅 Vulkan 后端在设备支持
    // VK_KHR_acceleration_structure + VK_KHR_ray_query 时为真；GL/WebGL/D3D12
    // 一律为假。上层据此决定是否走 RT 反射、否则回退 SSR。
    bool supports_ray_tracing = false;
};

// ---------------------------------------------------------------------------
// IRenderBackend — 渲染后端抽象接口
// 注意：以 std::unique_ptr<IRenderBackend> 跨 DLL 边界传递，必须导出以保证
// vtable/析构函数在模块间一致。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // 生命周期
    virtual bool init(void* native_window) = 0;
    virtual void shutdown() = 0;

    // 上下文管理（把 GLFW/GL 的 make_current / swap_buffers 抽象出来，
    // 便于 Vulkan backend 实现空操作或 surface 相关逻辑）
    virtual void make_current(void* native_window) = 0;
    virtual void release_context() = 0;

    // 帧循环
    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    // 将已提交命令 flush 到驱动，不等待 GPU 完成（用于减少退出延迟）
    virtual void flush_gpu() {}

    // 强制 GPU 完成所有已提交命令（真正 idle，用于销毁资源/后端前）
    virtual void wait_gpu_idle() {}

    // 兼容旧名：默认委托给 wait_gpu_idle()
    virtual void finish_gpu() { wait_gpu_idle(); }

    // 状态
    virtual void clear(float r, float g, float b, float a) = 0;
    virtual void clear_depth() {}

    // 引擎纹理槽位（TextureSlots）→ 后端纹理单元。
    // 槽位表预留到 46，但 GL 的片段纹理单元上限可能只有 32（Intel 集显实测
    // exactly 32）：glActiveTexture(GL_TEXTURE0 + slot) 越界会返回
    // GL 后端在此把高位槽位压进空闲单元（同一 draw 内保持互不冲突）；
    // Vulkan 走描述符 binding，不受单元数限制，返回原值即可。
    virtual int texture_unit_for_slot(int slot) const { return slot; }
    virtual void set_viewport(int x, int y, int w, int h) = 0;
    virtual void set_viewport(int x, int y, int w, int h, uint32_t viewport_index) {
        (void)x; (void)y; (void)w; (void)h; (void)viewport_index;
    }
    virtual void set_scissor(int x, int y, int w, int h) {
        (void)x; (void)y; (void)w; (void)h;
    }
    virtual void set_scissor(int x, int y, int w, int h, uint32_t viewport_index) {
        (void)x; (void)y; (void)w; (void)h; (void)viewport_index;
    }
    virtual uint32_t max_viewports() const { return 1; }
    virtual void set_depth_test(bool enabled) = 0;
    // 深度写入开关（透明物体：测试开、写入关）。默认实现为空。
    virtual void set_depth_write(bool enabled) { (void)enabled; }
    virtual void set_blend(bool enabled) = 0;
    virtual void set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) {
        (void)src_factor; (void)dst_factor;
    }
    virtual void set_blend_equation(BlendEquation mode) { (void)mode; }
    virtual void set_cull_face(CullMode mode) = 0;

    // 多边形模式（线框显示）。只有 OpenGL/WebGL 后端实现：Vulkan 需要
    // VK_EXT_extended_dynamic_state3 的 dynamic polygonMode，D3D12 需要为每个
    // 着色器准备 wireframe 版 PSO。不支持的后端保持默认空实现（命令被丢弃），
    // 由 GRender_IsDisplayModeSupported 如实回报。
    virtual void set_polygon_mode(int mode) { (void)mode; }
    // 后端是否真的能把线框下推到硬件。GL 桌面版为真；WebGL 没有 polygonMode，
    // Vulkan/D3D12 未实现，一律为假 —— 编辑器据此禁用 Wireframe 选项而不是
    // 让用户点了个什么都不发生的开关。
    virtual bool supports_polygon_mode() const { return false; }

    // 绘制（使用 RHI 句柄）
    virtual void draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) = 0;
    virtual void draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) = 0;

    // ---- 计算着色器 / 间接绘制（可选能力，默认关闭且空实现）----
    // GPU-driven 渲染的基础设施：B0 阶段先以「默认空实现」加入接口，因此后端
    // 未实现时不会破坏任何现有行为（supports_compute_shaders() 返回 false）。
    // 逐步在 OpenGL / Vulkan 后端实现后即可支撑 GPU 剔除（遮挡剔除）与
    // vkCmdDrawIndexedIndirect / glDrawElementsIndirect 的间接提交。
    virtual bool supports_compute_shaders() const { return false; }

    // 硬件光追能力查询：默认关闭（GL/WebGL/D3D12 走默认）。Vulkan 后端在
    // 设备支持 VK_KHR_acceleration_structure + VK_KHR_ray_query 时覆盖为真。
    // 上层据此选择 RT 反射（可用时优先）或回退 SSR（屏幕空间反射）。
    virtual bool supports_ray_tracing() const { return false; }

    // 硬件光追反射（Vulkan ray query）应用入口。默认空实现：不支持光追或未开启
    // 的后端什么都不做，画面与现状零差异。Vulkan 后端覆盖：在支持设备上用 TLAS
    // 做 ray query，把反射合成回 color_tex。
    // color_tex/depth_tex = HDR 颜色 / 深度纹理句柄；view_proj/inv_view_proj =
    // 渲染用视图投影矩阵；camera_pos = 世界空间相机位置；pp 携带开关与参数。
    virtual void apply_ray_query_reflection(RHITextureHandle /*color_tex*/,
                                           RHITextureHandle /*depth_tex*/,
                                           int /*width*/, int /*height*/,
                                           const math::Matrix4f& /*view_proj*/,
                                           const math::Matrix4f& /*inv_view_proj*/,
                                           const math::Vector3f& /*camera_pos*/,
                                           const PostProcessParams& /*pp*/) {}

    // 创建计算管线：加载并编译 {shader_dir}/{name}.comp。
    // 不支持时返回无效句柄（is_valid() == false），调用方必须据此回退到 CPU 路径。
    virtual RHIShaderHandle create_compute_shader(const char* /*name*/,
                                                  const char* /*shader_dir*/) {
        return RHIShaderHandle{};
    }

    // 派发计算工作组：(gx, gy, gz) 为工作组数量。
    virtual void dispatch_compute(RHIShaderHandle shader, uint32_t gx, uint32_t gy, uint32_t gz) {
        (void)shader; (void)gx; (void)gy; (void)gz;
    }

    // 把 SSBO 绑定到计算/图形着色器的存储缓冲槽位（按名字绑定，跨后端统一）。
    virtual void bind_storage_buffer(RHIShaderHandle shader, const char* name, RHIBufferHandle buffer) {
        (void)shader; (void)name; (void)buffer;
    }

    // 把纹理整层拷到缓冲（GPU 端，不做 CPU 映射、不产生同步停顿）。
    // 用途：compute 着色器没有 sampled image 绑定能力时，把需要被 compute 读取的
    // 渲染结果（HiZ 深度金字塔等）搬到 SSBO 里按索引读取。
    // offset 单位为字节。数据布局为逐行紧密排列（row = width * sizeof(texel)）。
    virtual void copy_texture_to_buffer(RHITextureHandle /*texture*/, RHIBufferHandle /*buffer*/,
                                        uint32_t /*offset*/) {}

    // 显式关闭当前 render pass（若开着），使紧随其后的 copy/dispatch 类命令可以
    // 在 pass 之外合法录制。Vulkan 不允许 render pass 内出现 transfer/compute 命令；
    // OpenGL / D3D12 没有 render pass 概念，默认空实现即可。
    // 调用方须知：关闭后必须重新绑定 framebuffer 才能继续绘制。
    virtual void end_render_pass_for_transfer() {}

    // ---- 纹理探针（可选能力，默认空实现）----
    // 把指定纹理拷回 CPU 并输出统计信息（均值 / 非零占比 / 内容哈希），
    // 用于比对同一场景在不同后端、不同次运行下是否算出相同结果
    //（间歇性画面差异的定位手段）。实现方自行门控（环境变量），对正常
    // 渲染路径零影响。
    virtual void probe_texture(RHITextureHandle /*tex*/, const char* /*tag*/) {}

    // 指定后续间接绘制的"每实例属性流"（替代 mesh 自带的实例缓冲）。
    // GPU 剔除把可见实例矩阵紧凑写进 SSBO，Vulkan 侧直接把它当作 binding 1
    // 的实例顶点缓冲用——这样图形着色器无需改动就能画出剔除结果，instanceCount
    // 由间接参数给出。OpenGL 不需要：它由顶点着色器直接读 SSBO。
    // 传无效句柄表示恢复使用 mesh 自身的实例缓冲。
    virtual void bind_instance_stream(RHIBufferHandle buffer) { (void)buffer; }

    // 间接绘制：绘制参数（indexCount / instanceCount / firstIndex 等）来自 GPU 缓冲，
    // CPU 不再参与逐物体提交，从而把提交开销压到接近零。
    virtual void draw_indexed_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                                       RHIBufferHandle args, uint32_t offset) {
        (void)mesh; (void)shader; (void)args; (void)offset;
    }

    // 非索引版间接绘制（DrawArraysIndirectCommand：count / instanceCount /
    // first / baseInstance）。实例化几何在上传时被去索引展开，不走索引分支。
    virtual void draw_arrays_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                                      RHIBufferHandle args, uint32_t offset) {
        (void)mesh; (void)shader; (void)args; (void)offset;
    }

    // 工厂方法：创建后端特定资源，返回句柄
    virtual RHIMeshHandle create_mesh() = 0;
    virtual RHIShaderHandle create_shader() = 0;
    virtual RHITextureHandle create_texture() = 0;
    virtual RHIFramebufferHandle create_framebuffer() = 0;
    virtual RHIBufferHandle create_buffer() = 0;

    // 销毁资源（渲染线程安全）
    virtual void destroy_mesh(RHIMeshHandle handle) = 0;
    virtual void destroy_shader(RHIShaderHandle handle) = 0;
    virtual void destroy_texture(RHITextureHandle handle) = 0;
    virtual void destroy_framebuffer(RHIFramebufferHandle handle) = 0;
    virtual void destroy_buffer(RHIBufferHandle handle) = 0;

    // 通过句柄访问实际对象（供渲染线程使用）
    virtual IMesh* mesh(RHIMeshHandle handle) = 0;
    virtual IShader* shader(RHIShaderHandle handle) = 0;
    virtual ITexture* texture(RHITextureHandle handle) = 0;
    virtual IFramebuffer* framebuffer(RHIFramebufferHandle handle) = 0;
    virtual IBuffer* buffer(RHIBufferHandle handle) = 0;

    // 帧缓冲
    virtual void bind_framebuffer(RHIFramebufferHandle fb) { (void)fb; }
    virtual void unbind_framebuffer() {}

    // MSAA 解析：把 src 帧缓冲的多样本颜色附件解析到 dst 的单样本颜色附件。
    // 主 HDR 目标启用 MSAA 时，场景 pass 结束后调用一次，后续 pass 无缝
    // 采样解析结果（hdr_color_ 保持单样本形态，下游零改动）。
    // 两端都不是多样本时后端可直接忽略。在渲染线程的命令流里执行。
    virtual void resolve_framebuffer(RHIFramebufferHandle src, RHIFramebufferHandle dst) {
        (void)src; (void)dst;
    }

    // 帧率 / 呈现控制
    virtual void set_swap_interval(int interval) { (void)interval; }
    virtual void set_gpu_busy_spin(bool enabled, int iterations) {
        (void)enabled; (void)iterations;
    }
    virtual void set_nv_delay_before_swap(float seconds) { (void)seconds; }
    virtual bool supports_nv_delay_before_swap() const { return false; }

    // 信息
    virtual const char* api_name() const = 0;
    virtual const char* api_version() const = 0;

    // ---- GPU 分段计时（可选能力，默认关闭且空实现）----
    // 用于回答"这一帧的 GPU 时间花在哪一段"，而不是靠分辨率/开关推断。
    //   begin/end 必须成对、按顺序、**不支持嵌套**（GL 的 TIME_ELAPSED query
    //   虽然允许嵌套，但为了两个后端行为一致，这里按平铺的 pass 段使用）；
    //   begin 时若上一段未结束，该段会被忽略并只警告一次。
    //   frame_boundary 由后端在"该帧的 fence 已被等待"之后调用，用于结算上一轮
    //   结果（不阻塞 GPU），pipeline 不需要关心。
    virtual bool gpu_profile_supported() const { return false; }
    virtual void set_gpu_profiling(bool enabled) { (void)enabled; }
    virtual void gpu_profile_begin(const char* name) { (void)name; }
    virtual void gpu_profile_end() {}
    virtual void gpu_profile_frame_boundary() {}
    // 打印累计结果（按累计帧数求平均），print 后不清空。
    virtual void gpu_profile_dump() {}
    virtual void gpu_profile_reset() {}
    // GPU 总耗时读数（摊到每帧，ms；自开启计时以来的累计平均）。
    // 供调试 UI 显示"GPU 帧时间"；未开启计时或不支持时返回 0。
    virtual double gpu_frame_time_ms() const { return 0.0; }

    // 能力查询（将 vendor-specific 特性、格式支持等显式化）
    virtual RenderBackendCapabilities get_capabilities() const = 0;

    // Screenshot: backend captures current swapchain/frontbuffer to a file.
    virtual void request_screenshot(const std::string& path) { (void)path; }

    // Capture the current frame to a file immediately (called from render thread).
    virtual void capture_frame_to_file(const std::string& path) { (void)path; }

    // Synchronous readback of the current framebuffer/swapchain as top-down RGBA.
    // Returns true on success; out is resized to w*h*4.
    virtual bool capture_frame_rgba(std::vector<uint8_t>& out, int& w, int& h) {
        (void)out; (void)w; (void)h;
        return false;
    }

    // Factory for 2D renderer (OpenGL -> Renderer2D, Vulkan -> VulkanRenderer2D).
    virtual std::unique_ptr<IRenderer2D> create_renderer2d() { return nullptr; }

    // Factory for ImGui render backend.
    virtual std::unique_ptr<IImGuiBackend> create_imgui_backend() { return nullptr; }

    // 本后端的纹理/render-target 采样约定：纹理第 0 行是图像的**下**边时为 true
    // （OpenGL 的 framebuffer 原点在左下，Vulkan/D3D12 在左上）。
    //
    // 这个判据同时服务两条 UI 轨的 GUi_Image：ImGui 轨经 IImGuiBackend::
    // texture_needs_v_flip 间接拿到（那边是 ImGui 自己的覆写），内置轨直接问后端
    // —— 内置轨不建 ImGui 后端，拿不到前者。新增后端时两处必须一起改。
    virtual bool texture_needs_v_flip() const { return false; }

    // Enable backend validation (Vulkan validation layers). Must be called before init().
    virtual void set_validation_enabled(bool enabled) { (void)enabled; }
};

// 渲染后端分层：
//   Vulkan —— 默认后端（首选，全功能）
//   OpenGL —— 兼容后端（旧硬件兼容 / 排障对照）
//   DX11 / DX12 —— WinNative 预留，尚未实现（工厂返回 nullptr）
//   WebGL —— Emscripten 构建专用：与 OpenGL 共用 GLBackend 实现，
//            差异（DSA shim / GLES3 头 / ES300 着色器）全部关在
//            #ifdef __EMSCRIPTEN__ 内，桌面构建零影响
enum class RenderAPI {
    Vulkan,
    OpenGL,
    DX11,
    DX12,
    WebGL
};

// 根据 API 创建对应后端；DX11/DX12 为预留项，当前返回 nullptr。
GRYCE_RENDERER_API std::unique_ptr<IRenderBackend> create_render_backend(RenderAPI api);

} // namespace gryce_engine::render
