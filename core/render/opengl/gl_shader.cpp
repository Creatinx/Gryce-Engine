#include "gl_shader.h"

#include "gl_utils.h"
#include <GL/glew.h>

#include <GLFW/glfw3.h>

#include <fstream>
#include <sstream>

#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

GLShader::GLShader() : program_id_(0) {}

GLShader::~GLShader() {
    if (program_id_) {
        glDeleteProgram(program_id_);
    }
}

namespace {

// 当前绑定的 OpenGL program，用于避免重复的 glUseProgram 调用。
thread_local GLuint g_current_bound_program = 0;

uint32_t to_gl_shader_stage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:   return GL_VERTEX_SHADER;
    case ShaderStage::Fragment: return GL_FRAGMENT_SHADER;
    case ShaderStage::Geometry: return GL_GEOMETRY_SHADER;
    case ShaderStage::Compute:  return GL_COMPUTE_SHADER;
    }
    return GL_VERTEX_SHADER;
}

const char* stage_name(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:   return "Vertex";
    case ShaderStage::Fragment: return "Fragment";
    case ShaderStage::Geometry: return "Geometry";
    case ShaderStage::Compute:  return "Compute";
    }
    return "Unknown";
}

} // namespace

bool GLShader::compile(const std::string& vertex_src,
                       const std::string& fragment_src) {
    std::vector<ShaderStageDesc> stages;
    stages.emplace_back(ShaderStage::Vertex, vertex_src);
    stages.emplace_back(ShaderStage::Fragment, fragment_src);
    return compile(stages);
}

bool GLShader::compile(const std::vector<ShaderStageDesc>& stages) {
    if (program_id_) {
        glDeleteProgram(program_id_);
        program_id_ = 0;
    }
    uniform_cache_.clear();

    std::vector<uint32_t> shader_ids;
    shader_ids.reserve(stages.size());

    int success = 0;
    for (const auto& stage : stages) {
        uint32_t shader = glCreateShader(to_gl_shader_stage(stage.stage));
        const char* src = stage.source.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(shader, 512, nullptr, log);
            GLOG_ERROR("{} shader compile failed: {}", stage_name(stage.stage), log);
            glDeleteShader(shader);
            for (uint32_t sid : shader_ids) {
                glDeleteShader(sid);
            }
            return false;
        }
        shader_ids.push_back(shader);
    }

    program_id_ = glCreateProgram();
    for (uint32_t shader : shader_ids) {
        glAttachShader(program_id_, shader);
    }
    glLinkProgram(program_id_);

    glGetProgramiv(program_id_, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program_id_, 512, nullptr, log);
        GLOG_ERROR("Shader link failed: {}", log);
        glDeleteProgram(program_id_);
        program_id_ = 0;
        for (uint32_t shader : shader_ids) {
            glDeleteShader(shader);
        }
        return false;
    }

    for (uint32_t shader : shader_ids) {
        glDeleteShader(shader);
    }

    GLOG_INFO("Shader compiled successfully, program={}", program_id_);
    GL_CHECK_ERROR();
    return true;
}

void GLShader::bind() const {
    if (program_id_ && program_id_ != g_current_bound_program) {
        glUseProgram(program_id_);
        g_current_bound_program = program_id_;
    }
    GL_CHECK_ERROR();
    apply_post_process_params();
}

void GLShader::unbind() const {
    if (g_current_bound_program != 0) {
        glUseProgram(0);
        g_current_bound_program = 0;
    }
    GL_CHECK_ERROR();
}

int GLShader::get_uniform_location(const char* name) const {
    return glGetUniformLocation(program_id_, name);
}

int GLShader::get_uniform_location_cached(const char* name) const {
    if (!program_id_) return -1;
    auto it = uniform_cache_.find(name);
    if (it != uniform_cache_.end()) {
        return it->second;
    }
    int loc = glGetUniformLocation(program_id_, name);
    uniform_cache_.emplace(name, loc);
    return loc;
}

void GLShader::set_int(const std::string& name, int value) {
    set_int(name.c_str(), value);
}

void GLShader::set_int(const char* name, int value) {
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_int: uniform '{}' not found (location={})", name, loc);
        return;
    }
    glUniform1i(loc, value);
    GL_CHECK_ERROR();
}

void GLShader::set_float(const std::string& name, float value) {
    set_float(name.c_str(), value);
}

void GLShader::set_float(const char* name, float value) {
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_float: uniform '{}' not found (location={})", name, loc);
        return;
    }
    glUniform1f(loc, value);
    GL_CHECK_ERROR();
}

void GLShader::set_vec2(const std::string& name, const gryce_engine::math::Vector2f& value) {
    set_vec2(name.c_str(), value);
}

void GLShader::set_vec2(const char* name, const gryce_engine::math::Vector2f& value) {
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_vec2: uniform '{}' not found (location={})", name, loc);
        return;
    }
    glUniform2f(loc, value.x, value.y);
    GL_CHECK_ERROR();
}

void GLShader::set_vec3(const std::string& name, const gryce_engine::math::Vector3f& value) {
    set_vec3(name.c_str(), value);
}

void GLShader::set_vec3(const char* name, const gryce_engine::math::Vector3f& value) {
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_vec3: uniform '{}' not found (location={})", name, loc);
        return;
    }
    glUniform3f(loc, value.x, value.y, value.z);
    GL_CHECK_ERROR();
}

void GLShader::set_vec4(const std::string& name, const gryce_engine::math::Vector4f& value) {
    set_vec4(name.c_str(), value);
}

void GLShader::set_vec4(const char* name, const gryce_engine::math::Vector4f& value) {
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_vec4: uniform '{}' not found (location={})", name, loc);
        return;
    }
    glUniform4f(loc, value.x, value.y, value.z, value.w);
    GL_CHECK_ERROR();
}

void GLShader::set_mat4(const std::string& name, const gryce_engine::math::Matrix4f& value) {
    set_mat4(name.c_str(), value);
}

void GLShader::set_mat4(const char* name, const gryce_engine::math::Matrix4f& value) {
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_mat4: uniform '{}' not found (location={})", name, loc);
        return;
    }
    glUniformMatrix4fv(loc, 1, GL_FALSE, value.m);
    GL_CHECK_ERROR();
}

bool GLShader::is_valid() const {
    return program_id_ != 0;
}

namespace {

std::string load_file_text(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace

bool GLShader::load_program(const std::string& name,
                            const std::string& shader_dir,
                            IFramebuffer* /*target*/,
                            bool /*color_output*/,
                            bool /*post_process*/) {
    std::string dir = resources::ResourcePath::resolve(shader_dir);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
        dir += '/';
    }

    std::string vertex_src = load_file_text(dir + name + ".vert");
    std::string fragment_src = load_file_text(dir + name + ".frag");
    if (vertex_src.empty() || fragment_src.empty()) {
        GLOG_ERROR("GLShader::load_program: failed to load '{}.vert' or '{}.frag' from '{}'", name, name, dir);
        return false;
    }
    return compile(vertex_src, fragment_src);
}

void GLShader::set_post_process_params(float exposure, int mode) {
    pp_exposure_ = exposure;
    pp_mode_ = mode;
    pp_dirty_ = true;
}

void GLShader::apply_post_process_params() const {
    if (!pp_dirty_ || program_id_ == 0) return;
    int exposure_loc = get_uniform_location("uExposure");
    int mode_loc = get_uniform_location("uToneMapMode");
    if (exposure_loc >= 0) {
        glUniform1f(exposure_loc, pp_exposure_);
    }
    if (mode_loc >= 0) {
        glUniform1i(mode_loc, pp_mode_);
    }
    pp_dirty_ = false;
}

} // namespace gryce_engine::render