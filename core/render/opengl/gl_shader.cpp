#include "gl_shader.h"

#include "gl_utils.h"
#include <GL/glew.h>

#include <GLFW/glfw3.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <system_error>

#include "assets/asset_manager.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

// 当前绑定的 OpenGL program，用于避免重复的 glUseProgram 调用。
// 声明必须在析构之前：~GLShader 需要失效该缓存。
thread_local GLuint g_current_bound_program = 0;

} // namespace

GLShader::GLShader() : program_id_(0) {}

GLShader::~GLShader() {
    if (program_id_) {
        // 删除前失效绑定缓存，避免驱动复用同一 program id 后
        // bind() 误判"已绑定"而跳过 glUseProgram。
        if (g_current_bound_program == program_id_) {
            g_current_bound_program = 0;
        }
        glDeleteProgram(program_id_);
    }
}

namespace {

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

// 从源码链接一个新 program（不触碰已有 program_id_）。
// 成功返回新 program id，失败返回 0。用于热重载：先编译新程序，
// 成功后再替换旧程序，避免重编译失败时破坏当前可用的 shader。
uint32_t link_program(const std::string& vertex_src, const std::string& fragment_src) {
    std::vector<ShaderStageDesc> stages;
    stages.emplace_back(ShaderStage::Vertex, vertex_src);
    stages.emplace_back(ShaderStage::Fragment, fragment_src);

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
            return 0;
        }
        shader_ids.push_back(shader);
    }

    uint32_t program = glCreateProgram();
    for (uint32_t shader : shader_ids) {
        glAttachShader(program, shader);
    }
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        GLOG_ERROR("Shader link failed: {}", log);
        glDeleteProgram(program);
        program = 0;
    }

    for (uint32_t shader : shader_ids) {
        glDeleteShader(shader);
    }
    return program;
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
        // 重编译删除旧 program 前失效绑定缓存（同析构）
        if (g_current_bound_program == program_id_) {
            g_current_bound_program = 0;
        }
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
    if (!name) return;
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
    if (!name) return;
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_float: uniform '{}' not found (program={}, location={})",
                  name, program_id_, loc);
        return;
    }
    glUniform1f(loc, value);
    GL_CHECK_ERROR();
}

void GLShader::set_vec2(const std::string& name, const gryce_engine::math::Vector2f& value) {
    set_vec2(name.c_str(), value);
}

void GLShader::set_vec2(const char* name, const gryce_engine::math::Vector2f& value) {
    if (!name) return;
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
    if (!name) return;
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
    if (!name) return;
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_vec4: uniform '{}' not found (program={}, location={})",
                  name, program_id_, loc);
        return;
    }
    glUniform4f(loc, value.x, value.y, value.z, value.w);
    GL_CHECK_ERROR();
}

void GLShader::set_mat4(const std::string& name, const gryce_engine::math::Matrix4f& value) {
    set_mat4(name.c_str(), value);
}

void GLShader::set_mat4(const char* name, const gryce_engine::math::Matrix4f& value) {
    if (!name) return;
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_mat4: uniform '{}' not found (program={}, location={})",
                  name, program_id_, loc);
        return;
    }
    glUniformMatrix4fv(loc, 1, GL_FALSE, value.m);
    GL_CHECK_ERROR();
}

void GLShader::set_mat4_array(const char* name, const gryce_engine::math::Matrix4f* data,
                              uint32_t count) {
    if (!name || !data || count == 0) return;
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        GLOG_WARN("GLShader::set_mat4_array: uniform '{}' not found (location={})", name, loc);
        return;
    }
    // Matrix4f::m 为连续 16 float（列主序），数组等价于连续 mat4 块
    glUniformMatrix4fv(loc, static_cast<GLsizei>(count), GL_FALSE, data[0].m);
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
                            bool /*post_process*/,
                            bool /*skybox*/,
                            bool /*skinned*/) {
    std::string dir = resources::ResourcePath::resolve(shader_dir);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
        dir += '/';
    }

    // Prefer the res:/ form so shader sources can be read from mounted
    // .gpack/.gpkg bundles; fall back to the resolved path for plain dirs.
    auto load_shader_source = [&](const char* ext) {
        std::string res_path = shader_dir;
        if (!res_path.empty() && res_path.back() != '/') res_path += '/';
        res_path += name;
        res_path += ext;
        std::string p = assets::AssetManager::instance().resolve_for_reading(res_path);
        if (!p.empty()) return load_file_text(p);
        return load_file_text(dir + name + ext);
    };

    std::string vertex_src = load_shader_source(".vert");
    std::string fragment_src = load_shader_source(".frag");
    if (vertex_src.empty() || fragment_src.empty()) {
        GLOG_ERROR("GLShader::load_program: failed to load '{}.vert' or '{}.frag' from '{}'", name, name, dir);
        return false;
    }

    // 记录源文件信息供热重载使用
    source_name_ = name;
    source_dir_ = dir;
    std::error_code ec;
    vert_mtime_ = std::filesystem::last_write_time(dir + name + ".vert", ec);
    frag_mtime_ = std::filesystem::last_write_time(dir + name + ".frag", ec);

    return compile(vertex_src, fragment_src);
}

bool GLShader::shader_files_changed() const {
    if (source_name_.empty() || source_dir_.empty()) return false;
    std::error_code ec;
    auto vert_mtime = std::filesystem::last_write_time(source_dir_ + source_name_ + ".vert", ec);
    if (ec) return false;
    auto frag_mtime = std::filesystem::last_write_time(source_dir_ + source_name_ + ".frag", ec);
    if (ec) return false;
    return vert_mtime != vert_mtime_ || frag_mtime != frag_mtime_;
}

bool GLShader::reload() {
    if (source_name_.empty() || source_dir_.empty()) return false;

    std::string vertex_src = load_file_text(source_dir_ + source_name_ + ".vert");
    std::string fragment_src = load_file_text(source_dir_ + source_name_ + ".frag");
    if (vertex_src.empty() || fragment_src.empty()) {
        GLOG_ERROR("GLShader::reload: failed to re-read '{}'", source_name_);
        return false;
    }

    // 先编译新程序，成功后再替换，避免失败时丢失当前可用程序
    uint32_t new_program = link_program(vertex_src, fragment_src);
    if (new_program == 0) {
        GLOG_ERROR("GLShader::reload: recompile failed for '{}', keeping old program", source_name_);
        return false;
    }

    if (program_id_) {
        if (g_current_bound_program == program_id_) {
            g_current_bound_program = 0;
        }
        glDeleteProgram(program_id_);
    }
    program_id_ = new_program;
    uniform_cache_.clear();
    pp_dirty_ = true; // 重编译后需重新应用 post-process 参数

    std::error_code ec;
    vert_mtime_ = std::filesystem::last_write_time(source_dir_ + source_name_ + ".vert", ec);
    frag_mtime_ = std::filesystem::last_write_time(source_dir_ + source_name_ + ".frag", ec);

    GLOG_INFO("GLShader: hot-reloaded '{}' (program={})", source_name_, program_id_);
    return true;
}

void GLShader::set_post_process_params(const PostProcessParams& params) {
    pp_params_ = params;
    pp_dirty_ = true;
}

void GLShader::apply_post_process_params() const {
    if (!pp_dirty_ || program_id_ == 0) return;
    const PostProcessParams& p = pp_params_;
    struct Uniform1f { const char* name; float value; };
    static const Uniform1f floats[] = {
        {"uExposure", p.exposure},
        {"uEV100", p.ev100},
        {"uWhitePoint", p.white_point},
        {"uBlackPoint", p.black_point},
        {"uContrast", p.contrast},
        {"uSaturation", p.saturation},
        {"uBloomThreshold", p.bloom_threshold},
        {"uBloomIntensity", p.bloom_intensity},
        {"uFilmGrain", p.film_grain},
        {"uVignette", p.vignette},
        {"uChromaticAberration", p.chromatic_aberration},
        {"uLUTStrength", p.lut_strength},
        {"uAETargetLuminance", p.ae_target_luminance},
        {"uAEMinExposure", p.ae_min_exposure},
        {"uAEMaxExposure", p.ae_max_exposure},
        {"uAESpeed", p.ae_speed},
        {"uTAAWeight", p.taa_weight},
        {"uSSAOStrength", p.ssao_strength},
        {"uSSAORadius", p.ssao_radius},
        {"uSSAONear", p.ssao_near},
        {"uSSAOFar", p.ssao_far},
        {"uSSAOTanHalfFov", p.ssao_tan_half},
        {"uSSAOAspect", p.ssao_aspect},
    };
    for (const auto& u : floats) {
        int loc = get_uniform_location(u.name);
        if (loc >= 0) glUniform1f(loc, u.value);
    }
    struct Uniform1i { const char* name; int value; };
    static const Uniform1i ints[] = {
        {"uToneMapMode", p.tone_map_mode},
        {"uDithering", p.dithering},
        {"uBloomEnabled", p.bloom_enabled},
        {"uUseLUT", p.use_lut},
        {"uAutoExposure", p.auto_exposure},
        {"uTAAEnabled", p.taa_enabled},
        {"uSSAOEnabled", p.ssao_enabled},
    };
    for (const auto& u : ints) {
        int loc = get_uniform_location(u.name);
        if (loc >= 0) glUniform1i(loc, u.value);
    }
    struct Uniform4f { const char* name; math::Vector4f value; };
    static const Uniform4f vec4s[] = {
        {"uLift", p.lift},
        {"uGamma", p.gamma},
        {"uGain", p.gain},
        {"uShadows", p.shadows},
        {"uMidtones", p.midtones},
        {"uHighlights", p.highlights},
    };
    for (const auto& u : vec4s) {
        int loc = get_uniform_location(u.name);
        if (loc >= 0) glUniform4f(loc, u.value.x, u.value.y, u.value.z, u.value.w);
    }
    pp_dirty_ = false;
}

} // namespace gryce_engine::render
