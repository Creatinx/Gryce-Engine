#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
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
    void set_post_process_params(const PostProcessParams& params) override;

    bool shader_files_changed() const override;
    bool reload() override;

    // 计算着色器（单阶段 *.comp）：编译失败返回 false。
    bool load_compute(const std::string& name, const std::string& shader_dir) override;
    int storage_block_binding(const char* name) const override;

    uint32_t program_id() const { return program_id_; }

private:
    uint32_t program_id_ = 0;

    // Shader 热重载：load_program 记录的源文件信息（resolved 目录 + 最后修改时间）
    std::string source_name_;
    std::string source_dir_;
    // 实际命中的源文件可读路径（磁盘或 bundle 解出的临时路径），供 mtime 跟踪
    std::string source_vert_path_;
    std::string source_frag_path_;
    std::filesystem::file_time_type vert_mtime_{};
    std::filesystem::file_time_type frag_mtime_{};

    // 计算着色器（load_compute）：单阶段，无 vert/frag
    std::string source_comp_path_;
    std::filesystem::file_time_type comp_mtime_{};

    mutable PostProcessParams pp_params_;
    mutable bool pp_dirty_ = true;

    // Uniform 位置缓存：避免每帧重复查询 driver。
    mutable std::unordered_map<std::string, int> uniform_cache_;

    // 已经告警过的"找不到的 uniform"：前向 shader 上会无条件写一批只属于
    // 延迟管线的 uniform（uGIMode / uShadowAtlasEnabled / uUseSSIL …），
    // 每帧 20+ 条告警会把日志和帧率一起拖垮（实测 124 帧 2760 条）。
    // 告警只留第一条，之后静默跳过。
    mutable std::unordered_set<std::string> missing_uniform_warned_;
    void warn_missing_uniform(const char* setter, const char* name) const;

    // SSBO block 名 → layout binding 缓存：storage_block_binding 原先每次调用都
    // 两次 glGetProgramResource*（驱动查询）；program 生命周期内绑定不变，
    // 热重载时统一清空。GPU 剔除路径每批每帧查 5-6 次，收益明显。
    mutable std::unordered_map<std::string, int> storage_block_binding_cache_;

    // 采样器 uniform 名字集合（由 program 反射得到）：set_int 需要据此判断
    // 写入的是「引擎槽位」还是普通整数，前者要按 gl_slot_map.h 换算成
    // 真正绑定的纹理单元，否则采样器指向未绑定的单元，采样恒为 0。
    mutable std::unordered_map<std::string, bool> sampler_uniform_cache_;
    mutable bool sampler_uniform_scanned_ = false;

    int get_uniform_location(const char* name) const;
    int get_uniform_location_cached(const char* name) const;
    void scan_sampler_uniforms() const;
    bool is_sampler_uniform(const char* name) const;
    void apply_post_process_params() const;
};

} // namespace gryce_engine::render
