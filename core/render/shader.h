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

    // 后端相关纹理绑定（OpenGL 可忽略，Vulkan 用于更新 descriptor set）
    virtual void set_texture(int slot, ITexture* texture) { (void)slot; (void)texture; }

    // Load a shader program by base name from a shader directory.
    // OpenGL backend loads `{dir}/{name}.vert` and `{dir}/{name}.frag` as GLSL source.
    // Vulkan backend loads `{dir}/spirv/vulkan_{name}.vert.spv` and `{dir}/spirv/vulkan_{name}.frag.spv`.
    // target/color_output/post_process are used by Vulkan to build the pipeline.
    // skybox=true 时（Vulkan）构建天空盒管线：单 cubemap sampler、深度 LESS_OR_EQUAL、不写深度、不剔除。
    virtual bool load_program(const std::string& name,
                              const std::string& shader_dir,
                              IFramebuffer* target = nullptr,
                              bool color_output = true,
                              bool post_process = false,
                              bool skybox = false) { (void)skybox; return false; }

    // Set post-process parameters (exposure + tone map mode). Used by tonemap shader.
    virtual void set_post_process_params(float exposure, int mode) {}

    virtual bool is_valid() const = 0;
};

} // namespace gryce_engine::render
