// gl_shader.cpp — GLShader（IShader 的 OpenGL 实现）：程序加载与变体解析、
// uniform 位置缓存与按名设置、采样器 uniform 识别，以及缺失 uniform 的去重告警。
#include "gl_shader.h"

#include "gl_utils.h"
#include "gl_slot_map.h"
#include <GL/glew.h>

#include <GLFW/glfw3.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <system_error>

#include "assets/asset_manager.h"
#include "resources/resource_path.h"
#include "render/render.h"
#include "render/shader_source_resolver.h"
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

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// GLSL 330 core -> GLSL ES 300（WebGL2）机械源码转换
//
// 实测引擎 GL 侧 107 个 shader：101 个 `#version 330 core` + 6 个
// `#version 430 core`，且 gl_FragColor / texture2D / varying / attribute
// **零处使用**。330/430 -> 300 es 的差异有四项可机械处理：
//   1. 版本行改写（`#version 330 core` / `#version 430 core` ->
//      `#version 300 es`；ESSL 要求它必须是第一行，所以原版本行及其之前的
//      内容一律丢弃，见下）；
//   2. 精度限定符注入（ES 300 的 fragment shader 没有 float/int 默认精度，
//      不声明直接编译失败；sampler 默认 lowp 会毁掉 PBR 精度，显式 highp）；
//   3. **uniform 初始化器剥离**：ESSL 3.00 不允许（ANGLE 报 "cannot
//      initialize this type of qualifier"），桌面 GLSL 330 允许。引擎 78 处
//      uniform 带初始化器（tonemap / TAA / SSR 一票开关量）。
//   4. texture2D( -> texture(（防御性，引擎实测零使用）。
// 语义真正不同的 shader（SSBO 版 instanced.vert / scene_pbr_ssbo.frag）
// 不走本转换：前者由 webgl_instanced.vert 变体替代，后者在
// scene_shader_forward_clustered.cpp 里路由到 scene_pbr。
//
// 转换在加载时于内存中进行，**不复制** 101 个 shader 文件。
// ---------------------------------------------------------------------------

// 去掉行内注释（// 与 /* */），返回纯代码部分。用于在 uniform 声明行里
// 定位 '=' —— 否则 "// 0 = 顶点属性实例流" 这类注释会被误判成初始化器。
std::string strip_line_comment(const std::string& line) {
    std::size_t cut = std::string::npos;
    const std::size_t line_comment = line.find("//");
    const std::size_t block_begin = line.find("/*");
    if (line_comment != std::string::npos) cut = line_comment;
    if (block_begin != std::string::npos &&
        (cut == std::string::npos || block_begin < cut)) {
        cut = block_begin;
    }
    return cut == std::string::npos ? line : line.substr(0, cut);
}

// 剥离全局 uniform 声明的初始化器：
//   uniform float uExposure = 1.0;   ->   uniform float uExposure;
// 保留行尾注释不动。返回被剥离的 uniform 名列表（供告警）。
//
// 为什么不需要花括号深度跟踪：GLSL 里 `uniform` 不是合法的局部变量存储
// 限定符，任何以 `uniform` 开头的行必然是全局 uniform 声明。
std::vector<std::string> strip_uniform_initializers(std::string& src) {
    std::vector<std::string> stripped;
    std::string out;
    out.reserve(src.size());

    std::size_t pos = 0;
    while (pos < src.size()) {
        const std::size_t nl = src.find('\n', pos);
        const std::size_t end = (nl == std::string::npos) ? src.size() : nl;
        std::string line = src.substr(pos, end - pos);

        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line.compare(first, 8, "uniform ") == 0) {
            const std::string code = strip_line_comment(line);
            const std::size_t eq = code.find('=');
            const std::size_t semi = code.rfind(';');
            if (eq != std::string::npos && semi != std::string::npos && eq < semi) {
                // 取名字（最后一个标识符，去掉数组下标）。decl 形如
                // "uniform float uExposure "，尾部有 '=' 前的空格，先 trim。
                std::string decl = code.substr(first, eq - first);
                const std::size_t decl_end = decl.find_last_not_of(" \t\r");
                if (decl_end != std::string::npos) decl.resize(decl_end + 1);
                const std::size_t name_begin = decl.find_last_of(" \t");
                std::string name = (name_begin == std::string::npos)
                                       ? decl
                                       : decl.substr(name_begin + 1);
                const std::size_t bracket = name.find('[');
                if (bracket != std::string::npos) name = name.substr(0, bracket);

                const std::size_t slash = line.find("//");
                const std::size_t block = line.find("/*");
                std::size_t comment_at = std::string::npos;
                if (slash != std::string::npos) comment_at = slash;
                if (block != std::string::npos &&
                    (comment_at == std::string::npos || block < comment_at)) {
                    comment_at = block;
                }
                line = code.substr(0, eq) + ";" +
                       (comment_at == std::string::npos ? std::string()
                                                        : line.substr(comment_at));
                stripped.push_back(name);
            }
        }

        out += line;
        if (nl != std::string::npos) out += '\n';
        pos = (nl == std::string::npos) ? src.size() : nl + 1;
    }

    src.swap(out);
    return stripped;
}

std::string to_glsl_es300(const std::string& src, ShaderStage stage) {
    const std::string kPrecisionBlock =
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp samplerCube;\n"
        "precision highp sampler2DShadow;\n"
        "precision highp sampler2DArray;\n";

    // 1) 版本行改写。ESSL 规范与 ANGLE 都要求 `#version` 位于**第一行**
    //    （连前置注释都不允许，报 "#version directive must occur on the
    //    first line of the shader"）。因此无条件把 `#version 300 es` 放到
    //    输出最前面，原版本行及其之前的内容整段丢弃 —— 引擎 shader 那里
    //    只可能是空白；非空白则告警（项目覆盖 shader 有许可证头时会触发，
    //    丢弃不影响语义）。
    std::string out = "#version 300 es\n";
    out += kPrecisionBlock;

    // 5) 纹理单元预算宏。WebGL2 的 GL_MAX_TEXTURE_IMAGE_UNITS 在真实 GPU 上
    //    是 16（ANGLE/D3D11 上报 16，SwiftShader 上报 32），而 GLSL 的采样器
    //    计数发生在**编译期**：声明了就是一个纹理单元，运行时 if/常量折叠都
    //    省不掉。pbr 系着色器声明了 25 个采样器，在 16 上限的机器上 ANGLE
    //    直接拒绝链接：
    //      "FRAGMENT shader texture image units count exceeds
    //       MAX_TEXTURE_IMAGE_UNITS(16)"
    //    链接失败会让 RenderPipeline 退化成只清屏——画面只剩 ImGui。
    //    这里把四个"可降级特性"开关按 0 注入，着色器据此裁掉对应采样器
    //    （见各 .frag 的说明）。
    //
    //    注意注入的是 GRYCE_HAS_* 本身，不是"低纹理单元"标志再让 shader 分支：
    //    D3D12 的 vulkanize 会把源码里所有 `#define NAME 值` 重放到文件开头
    //    （不区分 #if 分支），同一名字出现 0/1 两个替换值就会让 shaderc 报
    //    "Macro redefined; different substitutions"。所以每个宏全仓只允许有
    //    一处定义，Web 侧的定义就在这里。
    out += "#define GRYCE_HAS_PCSS 0\n";
    out += "#define GRYCE_HAS_VSM 0\n";
    out += "#define GRYCE_HAS_GI 0\n";
    out += "#define GRYCE_HAS_POINT_SHADOW 0\n";

    const std::size_t version_pos = src.find("#version");
    if (version_pos == std::string::npos) {
        out += src;
    } else {
        std::size_t line_end = src.find('\n', version_pos);
        if (line_end == std::string::npos) line_end = src.size();
        else line_end += 1; // 跳过换行本身

        const std::string prefix = src.substr(0, version_pos);
        const std::size_t non_ws = prefix.find_first_not_of(" \t\r\n");
        if (non_ws != std::string::npos) {
            // 注释头（/* 或 // 开头）静默丢弃；其他内容说明源文件结构异常。
            const bool comment_header =
                prefix.compare(non_ws, 2, "/*") == 0 || prefix.compare(non_ws, 2, "//") == 0;
            if (!comment_header) {
                GLOG_WARN("ES300 conversion: {} bytes of non-comment content precede "
                          "#version and were dropped (ESSL requires #version on line 1)",
                          non_ws);
            }
        }
        out.append(src, line_end, src.size() - line_end);
    }

    // 2) uniform 初始化器剥离（ESSL 3.00 不支持）。
    //    引擎所有 uniform 都由 CPU 侧每帧写入（RenderPipeline /
    //    SceneShaderForwardClustered 的 uniform 缓存），剥离不改变行为。
    //    仍逐条告警：项目覆盖 shader 若依赖 shader 侧默认值而 CPU 没设，
    //    会在这里显形，而不是静默读成 0。
    std::vector<std::string> stripped = strip_uniform_initializers(out);
    if (!stripped.empty()) {
        std::string names;
        for (std::size_t i = 0; i < stripped.size(); ++i) {
            if (i) names += ", ";
            names += stripped[i];
        }
        GLOG_INFO("ES300 conversion: stripped {} uniform initializer(s) [{}] "
                  "(ESSL 3.00 forbids them; CPU side must set these)",
                  stripped.size(), names);
    }

    // 3) 防御性机械替换：引擎 shader 实测零使用，但项目覆盖 shader（用户
    //    自带 shaders/ 目录）可能用到旧式写法。texture2D 在 ES 300 已移除。
    if (src.find("texture2D(") != std::string::npos) {
        GLOG_WARN("ES300 conversion: 'texture2D(' rewritten to 'texture(' "
                  "(engine shaders do not use it; project override shader?)");
    }
    for (std::size_t p = out.find("texture2D("); p != std::string::npos;
         p = out.find("texture2D(", p)) {
        out.replace(p, 10, "texture(");
        p += 8;
    }

    // 4) 桌面专有写法的显式告警（出现即说明该 shader 需要人工迁移，
    //    不要等 WebGL 编译器的 cryptic 报错）。
    auto warn_if_found = [&](const char* token, const char* hint) {
        if (out.find(token) != std::string::npos) {
            GLOG_WARN("ES300 conversion: '{}' found in shader source; {}", token, hint);
        }
    };
    warn_if_found("gl_FragColor", "ES 300 uses user-declared out variables");
    warn_if_found("varying ", "ES 300 uses in/out");
    warn_if_found("attribute ", "ES 300 uses in");

    (void)stage;
    return out;
}
#endif // __EMSCRIPTEN__

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
#ifdef __EMSCRIPTEN__
        const std::string converted = to_glsl_es300(stage.source, stage.stage);
        const char* src = converted.c_str();
#else
        const char* src = stage.source.c_str();
#endif
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

// 单阶段计算程序（*.comp）。计算程序没有 rasterization 阶段，link 时不需要
// 顶点/片段；失败返回 0（热重载据此保留旧程序）。
uint32_t link_compute_program(const std::string& compute_src) {
    uint32_t shader = glCreateShader(GL_COMPUTE_SHADER);
#ifdef __EMSCRIPTEN__
    // WebGL2 没有 compute shader：正常不会走到这里（supports_compute=false
    // 时管线走 CPU 回退）。真走到就转换后交给编译器报明确的错。
    const std::string converted = to_glsl_es300(compute_src, ShaderStage::Compute);
    const char* src = converted.c_str();
#else
    const char* src = compute_src.c_str();
#endif
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        GLOG_ERROR("Compute shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }

    uint32_t program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        GLOG_ERROR("Compute shader link failed: {}", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(shader);
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
    sampler_uniform_cache_.clear();
    sampler_uniform_scanned_ = false;
    storage_block_binding_cache_.clear();

    std::vector<uint32_t> shader_ids;
    shader_ids.reserve(stages.size());

    int success = 0;
    for (const auto& stage : stages) {
        uint32_t shader = glCreateShader(to_gl_shader_stage(stage.stage));
#ifdef __EMSCRIPTEN__
        // WebGL2：GLSL 330/430 core -> ES 300 运行时转换（见 to_glsl_es300）
        const std::string converted = to_glsl_es300(stage.source, stage.stage);
        const char* src = converted.c_str();
#else
        const char* src = stage.source.c_str();
#endif
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

// 扫描 program 的活跃 uniform，记录哪些是采样器。
// 数组 uniform 在 GL 里叫 "uTex[0]"，两种拼写都登记一份。
void GLShader::scan_sampler_uniforms() const {
    if (sampler_uniform_scanned_) return;
    sampler_uniform_scanned_ = true;
    sampler_uniform_cache_.clear();
    if (!program_id_) return;

    auto is_sampler_type = [](GLenum type) {
        switch (type) {
            case GL_SAMPLER_1D:
            case GL_SAMPLER_2D:
            case GL_SAMPLER_3D:
            case GL_SAMPLER_CUBE:
            case GL_SAMPLER_1D_SHADOW:
            case GL_SAMPLER_2D_SHADOW:
            case GL_SAMPLER_CUBE_SHADOW:
            case GL_SAMPLER_1D_ARRAY:
            case GL_SAMPLER_2D_ARRAY:
            case GL_SAMPLER_1D_ARRAY_SHADOW:
            case GL_SAMPLER_2D_ARRAY_SHADOW:
            case GL_SAMPLER_2D_MULTISAMPLE:
            case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
            case GL_SAMPLER_CUBE_MAP_ARRAY:
            case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
            case GL_SAMPLER_BUFFER:
            case GL_INT_SAMPLER_1D:
            case GL_INT_SAMPLER_2D:
            case GL_INT_SAMPLER_3D:
            case GL_INT_SAMPLER_CUBE:
            case GL_INT_SAMPLER_2D_ARRAY:
            case GL_INT_SAMPLER_BUFFER:
            case GL_UNSIGNED_INT_SAMPLER_1D:
            case GL_UNSIGNED_INT_SAMPLER_2D:
            case GL_UNSIGNED_INT_SAMPLER_3D:
            case GL_UNSIGNED_INT_SAMPLER_CUBE:
            case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            case GL_UNSIGNED_INT_SAMPLER_BUFFER:
                return true;
            default:
                return false;
        }
    };

    GLint count = 0;
    glGetProgramiv(program_id_, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
        char name[256] = {};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program_id_, static_cast<GLuint>(i), sizeof(name) - 1,
                           &length, &size, &type, name);
        if (length <= 0 || !is_sampler_type(type)) continue;
        std::string full(name, static_cast<size_t>(length));
        sampler_uniform_cache_.emplace(full, true);
        // "uTex[0]" → 同时登记 "uTex"
        const size_t bracket = full.find('[');
        if (bracket != std::string::npos) {
            sampler_uniform_cache_.emplace(full.substr(0, bracket), true);
        }
    }
}

bool GLShader::is_sampler_uniform(const char* name) const {
    if (!name || !name[0]) return false;
    scan_sampler_uniforms();
    auto it = sampler_uniform_cache_.find(name);
    return it != sampler_uniform_cache_.end();
}

void GLShader::set_int(const std::string& name, int value) {
    set_int(name.c_str(), value);
}

void GLShader::warn_missing_uniform(const char* setter, const char* name) const {
    // 同一 (setter, uniform) 只告警一次。前向 shader 每帧都会被写入一批只属于
    // 延迟管线的 uniform，逐帧告警会同时拖慢 CPU 并淹没后续日志。
    std::string key(setter);
    key.push_back('|');
    key.append(name);
    if (!missing_uniform_warned_.insert(std::move(key)).second) return;
    GLOG_WARN("{}: uniform '{}' not declared by this shader (ignored; warning shown once)",
              setter, name);
}

void GLShader::set_int(const char* name, int value) {
    if (!name) return;
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        warn_missing_uniform("GLShader::set_int", name);
        return;
    }
    // 采样器 uniform：写入的是「引擎槽位」，必须换算成真正绑定的纹理单元。
    // 槽位 < 单元上限时换算结果就是自己，行为与参考工程一致。
    int unit = value;
    if (is_sampler_uniform(name)) {
        const int max_units = gl_max_texture_image_units();
        unit = gl_texture_unit_for_slot(value, max_units);
    }
    glUniform1i(loc, unit);
    GL_CHECK_ERROR();
}

void GLShader::set_float(const std::string& name, float value) {
    set_float(name.c_str(), value);
}

void GLShader::set_float(const char* name, float value) {
    if (!name) return;
    int loc = get_uniform_location_cached(name);
    if (loc < 0) {
        warn_missing_uniform("GLShader::set_float", name);
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
        warn_missing_uniform("GLShader::set_vec2", name);
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
        warn_missing_uniform("GLShader::set_vec3", name);
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
        warn_missing_uniform("GLShader::set_vec4", name);
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
        warn_missing_uniform("GLShader::set_mat4", name);
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
        warn_missing_uniform("GLShader::set_mat4_array", name);
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
    // 两级解析 + core 兜底：项目磁盘文件 → 已挂载 bundle → 引擎默认 shader 目录。
    // 这样空项目（无任何自定义 shader）也能拿到 core 全套默认 shader 开箱渲染。
    // Web 构建下 GL 后端就是 WebGL 后端：解析器据此优先取 webgl_ 前缀变体
    // （SSBO 等 ES3.0 缺失语义的替代实现，如 webgl_instanced.vert）。
#ifdef __EMSCRIPTEN__
    const RenderAPI resolve_api = RenderAPI::WebGL;
#else
    const RenderAPI resolve_api = RenderAPI::OpenGL;
#endif
    ShaderSourceSet src = resolve_shader_source(name, shader_dir, resolve_api);
    if (!src.valid()) {
        GLOG_ERROR("GLShader::load_program: failed to resolve '{}' in '{}' "
                   "(project disk/bundle/core engine dir)\n vert: '{}' frag: '{}'",
                   name, shader_dir, src.vertex_path, src.fragment_path);
        return false;
    }

    // 记录实际命中的源文件信息供 hot-reload 使用
    source_name_ = name;
    source_dir_ = src.vertex_path;
    source_vert_path_ = src.vertex_path;
    source_frag_path_ = src.fragment_path;

    std::error_code ec;
    vert_mtime_ = std::filesystem::last_write_time(src.vertex_path, ec);
    frag_mtime_ = std::filesystem::last_write_time(src.fragment_path, ec);

    return compile(src.vertex, src.fragment);
}

bool GLShader::load_compute(const std::string& name, const std::string& shader_dir) {
    // 与 load_program 同源解析（项目磁盘 → bundle → core 引擎目录），只是取 .comp 单阶段。
#ifdef __EMSCRIPTEN__
    const RenderAPI resolve_api = RenderAPI::WebGL;
#else
    const RenderAPI resolve_api = RenderAPI::OpenGL;
#endif
    ShaderStageSource src = resolve_shader_stage_source(name, shader_dir, ".comp", resolve_api);
    if (!src.ok()) {
        GLOG_ERROR("GLShader::load_compute: failed to resolve '{}.comp' in '{}'", name, shader_dir);
        return false;
    }

    source_name_ = name;
    source_dir_ = src.path;
    source_comp_path_ = src.path;
    std::error_code ec;
    comp_mtime_ = std::filesystem::last_write_time(src.path, ec);

    std::vector<ShaderStageDesc> stages;
    stages.emplace_back(ShaderStage::Compute, src.code);
    return compile(stages);
}

int GLShader::storage_block_binding(const char* name) const {
    if (!program_id_ || !name || !*name) return -1;

    auto it = storage_block_binding_cache_.find(name);
    if (it != storage_block_binding_cache_.end()) return it->second;

    GLuint block_index = glGetProgramResourceIndex(program_id_, GL_SHADER_STORAGE_BLOCK, name);
    if (block_index == GL_INVALID_INDEX) {
        storage_block_binding_cache_.emplace(name, -1);
        return -1;
    }

    // 取该 block 在着色器里声明的 layout(binding = N)
    GLenum prop = GL_BUFFER_BINDING;
    GLint binding = -1;
    GLsizei length = 0;
    glGetProgramResourceiv(program_id_, GL_SHADER_STORAGE_BLOCK, block_index,
                           1, &prop, 1, &length, &binding);
    storage_block_binding_cache_.emplace(name, static_cast<int>(binding));
    return static_cast<int>(binding);
}

bool GLShader::shader_files_changed() const {
    // 计算着色器：单阶段，只跟踪 .comp 的 mtime
    if (!source_comp_path_.empty() && source_vert_path_.empty()) {
        std::error_code ec;
        auto comp_mtime = std::filesystem::last_write_time(source_comp_path_, ec);
        return !ec && comp_mtime != comp_mtime_;
    }
    if (source_name_.empty() || source_vert_path_.empty() || source_frag_path_.empty()) {
        return false;
    }
    std::error_code ec;
    auto vert_mtime = std::filesystem::last_write_time(source_vert_path_, ec);
    if (ec) return false;
    auto frag_mtime = std::filesystem::last_write_time(source_frag_path_, ec);
    if (ec) return false;
    return vert_mtime != vert_mtime_ || frag_mtime != frag_mtime_;
}

bool GLShader::reload() {
    // 计算着色器：单阶段重编译
    if (!source_comp_path_.empty() && source_vert_path_.empty()) {
        std::string compute_src = load_file_text(source_comp_path_);
        if (compute_src.empty()) {
            GLOG_ERROR("GLShader::reload: failed to re-read compute '{}'", source_name_);
            return false;
        }
        uint32_t new_program = link_compute_program(compute_src);
        if (new_program == 0) {
            GLOG_ERROR("GLShader::reload: compute recompile failed for '{}', keeping old program",
                       source_name_);
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
        sampler_uniform_cache_.clear();
        sampler_uniform_scanned_ = false;
        storage_block_binding_cache_.clear();
        std::error_code ec;
        comp_mtime_ = std::filesystem::last_write_time(source_comp_path_, ec);
        return true;
    }

    if (source_name_.empty() || source_vert_path_.empty() || source_frag_path_.empty()) {
        return false;
    }

    std::string vertex_src = load_file_text(source_vert_path_);
    std::string fragment_src = load_file_text(source_frag_path_);
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
    sampler_uniform_cache_.clear();
    sampler_uniform_scanned_ = false;
    storage_block_binding_cache_.clear();
    pp_dirty_ = true; // 重编译后需重新应用 post-process 参数

    std::error_code ec;
    vert_mtime_ = std::filesystem::last_write_time(source_vert_path_, ec);
    frag_mtime_ = std::filesystem::last_write_time(source_frag_path_, ec);

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
    // 注意：绝不能是 static。静态局部数组只在首次调用时初始化一次，之后
    // 所有着色器都会一直使用"第一次那份参数"——表现为 SSAO/SSR 的相机参数
    //（uSSAOTanHalfFov 等）恒为 0：GTAO 里视图坐标重建退化、全屏判定为遮挡、
    // AO 输出 0，PBR 再用 ssao_factor 把环境光与 IBL 乘没，金属就变成了黑色。
    const Uniform1f floats[] = {
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
        {"uMotionBlurAmount", p.motion_blur_amount},
        {"uSSAOStrength", p.ssao_strength},
        {"uSSAORadius", p.ssao_radius},
        {"uSSAONear", p.ssao_near},
        {"uSSAOFar", p.ssao_far},
        {"uSSAOTanHalfFov", p.ssao_tan_half},
        {"uSSAOAspect", p.ssao_aspect},
        {"uSSRMaxRoughness", p.ssr_max_roughness},
        {"uSSRThickness", p.ssr_thickness},
        {"uSSRBilateralFilter", p.ssr_bilateral_filter},
        {"uSSREnvFallback", p.ssr_env_fallback},
        {"uSSRDebugMode", static_cast<float>(p.ssr_debug_view)},
        {"uSSRNear", p.ssr_near},
        {"uSSRFar", p.ssr_far},
        {"uSSRTanHalfFov", p.ssr_tan_half},
        {"uSSRAspect", p.ssr_aspect},
        // 接触阴影：pass 早就算好了，但这两个 uniform 一直没人写，
        // tonemap 里 uContactShadowEnabled 恒为 0 → 整条接触阴影等于没接上。
        {"uContactShadowStrength", p.cs_strength},
    };
    for (const auto& u : floats) {
        int loc = get_uniform_location(u.name);
        if (loc >= 0) glUniform1f(loc, u.value);
    }
    struct Uniform1i { const char* name; int value; };
    const Uniform1i ints[] = {
        {"uToneMapMode", p.tone_map_mode},
        {"uDithering", p.dithering},
        {"uBloomEnabled", p.bloom_enabled},
        {"uUseLUT", p.use_lut},
        {"uAutoExposure", p.auto_exposure},
        {"uTAAEnabled", p.taa_enabled},
        {"uSSAOEnabled", p.ssao_enabled},
        {"uSSREnabled", p.ssr_enabled},
        {"uSSRMaxSteps", p.ssr_max_steps},
        {"uMotionBlurEnabled", p.motion_blur_enabled},
        {"uContactShadowEnabled", p.cs_enabled},
    };
    for (const auto& u : ints) {
        int loc = get_uniform_location(u.name);
        if (loc >= 0) glUniform1i(loc, u.value);
    }
    struct Uniform4f { const char* name; math::Vector4f value; };
    // 同 floats/ints：绝不能是 static，否则只会保留第一次调用时的取值。
    const Uniform4f vec4s[] = {
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
