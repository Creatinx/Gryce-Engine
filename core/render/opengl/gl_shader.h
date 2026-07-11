#pragma once

#include <string>
#include <cstdint>
#include <vector>

#include "render/shader.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GLShader — OpenGL Shader 实现
// ---------------------------------------------------------------------------
class GLShader : public IShader {
public:
    GLShader();
    ~GLShader() override;

    bool compile(const std::string& vertex_src,
                 const std::string& fragment_src) override;
    bool compile(const std::vector<ShaderStageDesc>& stages) override;

    void bind() const override;
    void unbind() const override;

    void set_int(const std::string& name, int value) override;
    void set_int(const char* name, int value) override;
    void set_float(const std::string& name, float value) override;
    void set_float(const char* name, float value) override;
    void set_vec2(const std::string& name, const gryce_engine::math::Vector2f& value) override;
    void set_vec2(const char* name, const gryce_engine::math::Vector2f& value) override;
    void set_vec3(const std::string& name, const gryce_engine::math::Vector3f& value) override;
    void set_vec3(const char* name, const gryce_engine::math::Vector3f& value) override;
    void set_vec4(const std::string& name, const gryce_engine::math::Vector4f& value) override;
    void set_vec4(const char* name, const gryce_engine::math::Vector4f& value) override;
    void set_mat4(const std::string& name, const gryce_engine::math::Matrix4f& value) override;
    void set_mat4(const char* name, const gryce_engine::math::Matrix4f& value) override;

    bool is_valid() const override;

    bool load_program(const std::string& name,
                      const std::string& shader_dir,
                      IFramebuffer* target = nullptr,
                      bool color_output = true,
                      bool post_process = false) override;
    void set_post_process_params(float exposure, int mode) override;

    uint32_t program_id() const { return program_id_; }

private:
    uint32_t program_id_ = 0;

    mutable float pp_exposure_ = 1.0f;
    mutable int pp_mode_ = 1;
    mutable bool pp_dirty_ = true;

    int get_uniform_location(const char* name) const;
    void apply_post_process_params() const;
};

} // namespace gryce_engine::render