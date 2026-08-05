#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
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
    void set_mat4_array(const char* name, const gryce_engine::math::Matrix4f* data,
                        uint32_t count) override;

    bool is_valid() const override;

    bool load_program(const std::string& name,
                      const std::string& shader_dir,
                      IFramebuffer* target = nullptr,
                      bool color_output = true,
                      bool post_process = false,
                      bool skybox = false,
                      bool skinned = false) override;
    void set_post_process_params(float exposure, int mode) override;

    bool shader_files_changed() const override;
    bool reload() override;

    uint32_t program_id() const { return program_id_; }

private:
    uint32_t program_id_ = 0;

    // Shader 热重载：load_program 记录的源文件信息（resolved 目录 + 最后修改时间）
    std::string source_name_;
    std::string source_dir_;
    std::filesystem::file_time_type vert_mtime_{};
    std::filesystem::file_time_type frag_mtime_{};

    mutable float pp_exposure_ = 1.0f;
    mutable int pp_mode_ = 1;
    mutable bool pp_dirty_ = true;

    // Uniform 位置缓存：避免每帧重复查询 driver。
    mutable std::unordered_map<std::string, int> uniform_cache_;

    int get_uniform_location(const char* name) const;
    int get_uniform_location_cached(const char* name) const;
    void apply_post_process_params() const;
};

} // namespace gryce_engine::render