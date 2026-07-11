#include "render.h"

#include "render/opengl/gl_backend.h"
#ifdef GRYCE_HAS_VULKAN
#include "render/vulkan/vk_backend.h"
#endif
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

std::unique_ptr<IRenderBackend> create_render_backend(RenderAPI api) {
    switch (api) {
    case RenderAPI::OpenGL:
        return std::make_unique<GLBackend>();
    case RenderAPI::Vulkan:
#ifdef GRYCE_HAS_VULKAN
        return std::make_unique<VulkanBackend>();
#else
        GLOG_ERROR("create_render_backend: Vulkan support not compiled in");
        return nullptr;
#endif
    default:
        GLOG_ERROR("create_render_backend: unsupported RenderAPI {}", static_cast<int>(api));
        return nullptr;
    }
}

} // namespace gryce_engine::render
