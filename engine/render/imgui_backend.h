#pragma once

#include <cstdint>

#include "render/export.h"

struct ImDrawData;

namespace gryce_engine::render {

class ITexture;

// ---------------------------------------------------------------------------
// IImGuiBackend — Dear ImGui 渲染后端抽象
// 负责 API 相关的设备对象创建、每帧 NewFrame、绘制 DrawData。
// 平台相关部分（GLFW）由 ImGuiRenderer 统一处理。
//
// 注意：该类会被 std::unique_ptr<IImGuiBackend> 以值形式跨 DLL 边界返回，
// 因此必须导出，否则 MSVC/Windows 下会出现 ABI/vtable 不匹配，导致返回槽
// 地址为 null，进而在 unique_ptr 内部构造 _Compressed_pair 时崩溃。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API IImGuiBackend {
public:
    virtual ~IImGuiBackend() = default;

    // 初始化/销毁 API 设备对象
    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // 每帧调用（在 ImGui::NewFrame 之前）
    virtual void new_frame() = 0;

    // 绘制 ImGui 数据
    virtual void render_draw_data(ImDrawData* draw_data) = 0;

    // 是否使用 Vulkan（影响 GLFW 初始化方式）
    virtual bool is_vulkan() const { return false; }

    // 是否使用 Direct3D 12（影响 GLFW 初始化方式：ImGui_ImplGlfw_InitForOther）。
    virtual bool is_d3d12() const { return false; }

    // 将引擎纹理转换为 ImGui 用户纹理 ID（ImTextureID，供编辑器 Viewport
    // 面板 ImGui::Image 采样）。OpenGL 返回 GLuint 纹理对象 id，Vulkan 返回
    // 缓存的 VkDescriptorSet；返回 0 表示转换失败。
    virtual uint64_t imgui_texture_id(ITexture* texture) const {
        (void)texture;
        return 0;
    }

    // 采样本后端纹理时是否需要翻转 V。
    //
    // ImGui 的 Image() 默认 uv 是 (0,0)-(1,1)，即"图像数据的第一行在 v=0"
    // ——它自己的字体图集就是按这个约定上传的（后端不做翻转）。但不同后端的
    // "第一行"含义不同：
    //   OpenGL  渲染目标纹理的第一行是图像的**下**边（GL 原点在左下），
    //           文件贴图在 load_from_file 里也被翻成 bottom-up；
    //   D3D12 / Vulkan  资源的第一行就是图像的**上**边。
    // 所以只有 OpenGL 需要翻。漏了这个的症状是"编辑器视口里场景上下颠倒"：
    // 地面跑到天上、立方体的受光顶面显示在底边，而面板文字与图标一切正常
    // （文字走字体图集，与渲染目标纹理无关）。
    virtual bool texture_needs_v_flip() const { return false; }

    // 使指定纹理的 ImGui 缓存失效（如纹理被销毁/重建后）。
    virtual void invalidate_texture(ITexture* texture) { (void)texture; }

    // 运行时重建 ImGui 字体 GPU 纹理（如字体大小变化后）。
    // 调用方需保证当前线程持有正确的 GPU context / device。
    virtual void rebuild_fonts() {}
};

} // namespace gryce_engine::render
