#ifndef GRYCE_RENDER_OPENGL_GL_WEBGL_SHIM_H
#define GRYCE_RENDER_OPENGL_GL_WEBGL_SHIM_H

// ---------------------------------------------------------------------------
// WebGL2（Emscripten）适配层：让桌面 GL 后端零源码改动跑在浏览器里。
//
// 本文件**只在 __EMSCRIPTEN__ 下有意义**，且只经 cmake/web/GL/glew.h 这个
// <GL/glew.h> 替身被间接包含（Emscripten 构建时 cmake/web 加进 include path，
// 桌面构建完全不涉及本文件，也无任何源码改动）。
//
// 背景：WebGL2 = GLES 3.0，是桌面 GL 的严格子集。GL 后端（engine/render/
// opengl/，4,393 行）整体复用，差异只有三类：
//
//   1. 没有 GLEW —— Emscripten 用 GLES3/gl3.h 直映射 WebGL2，这里补齐
//      GLEW 的宏与 glewInit() 表面；
//   2. 没有 DSA（直接状态访问）—— 但实测 GL 后端**所有** DSA 调用都已包在
//      gl_dsa_available() 里（gl_buffer/gl_texture/gl_framebuffer 三处，
//      共 20+ 个门控点），而 WebGL2 版本串是 "OpenGL ES 3.0"，该函数恒为
//      false → DSA 路径是死代码。这里仍提供 bind-based 的真实现（防逻辑
//      误判时静默出错），唯一**未门控**的 DSA 入口是
//      Renderer2D::bind_default_texture_slot 的 glBindTextureUnit，
//      必须真实现（2D 默认 1x1 贴图绑定走它）；
//   3. 没有 compute / indirect / MSAA / SSBO / GPU 计时 —— 这些入口在 ES3.0
//      头文件里不存在，必须能编译；运行期由 RenderBackendCapabilities
//      声明能力缺失走既有 CPU 回退路径，这些函数永不应当被调用，故实现为
//      " loudly 报错一次"的占位（绝不静默返回成功值造假）。
//
// 实现手法：全部用**宏重命名**把调用点改到 gryce_webgl_* 命名空间外的
// shim 函数。宏重命名绝不会与 Emscripten 头文件里可能出现的同名声明冲突
// （宏在包含 GLES3 头之后、本文件末尾统一定义；shim 函数内部调用的都是
// 未被重命名的真实 ES3 函数）。
// ---------------------------------------------------------------------------

#ifdef __EMSCRIPTEN__

#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>

#include <cstring>
#include <string>
#include <unordered_map>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {
namespace webgl_detail {

// 扩展查询：WebGL2 的扩展名列表经 glGetString(GL_EXTENSIONS) 暴露
//（Emscripten 的 GL 库会把 JS 侧扩展列表转成空格分隔字符串）。
inline bool has_extension(const char* name) {
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!exts) return false;
    std::string all(exts);
    return all.find(name) != std::string::npos;
}

inline bool ext_anisotropic() {
    static bool checked = false;
    static bool available = false;
    if (!checked) {
        checked = true;
        available = has_extension("EXT_texture_filter_anisotropic");
    }
    return available;
}

// texture id -> target 注册表。
// glBindTextureUnit / glTextureParameteri 等 DSA 入口不携带 target，
// 而 bind-based 等价物必须知道 target。glCreateTextures 与 glBindTexture
// 两条创建路径都会登记（glBindTexture 被包装后写入）。
inline std::unordered_map<GLuint, GLenum>& texture_targets() {
    static std::unordered_map<GLuint, GLenum> targets;
    return targets;
}

inline GLenum texture_target_of(GLuint texture) {
    auto it = texture_targets().find(texture);
    return it != texture_targets().end() ? it->second : GL_TEXTURE_2D;
}

// target -> GL_TEXTURE_BINDING_* 查询枚举（用于保存/恢复绑定，保持
// DSA"不扰动当前绑定"的语义）。
inline GLenum binding_query_of(GLenum target) {
    switch (target) {
        case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
        case GL_TEXTURE_3D:       return GL_TEXTURE_BINDING_3D;
        default:                  return GL_TEXTURE_BINDING_2D;
    }
}

// 死代码占位：打一次错误日志后返回。用于 compute/indirect/MSAA/SSBO/VAO
// 分离格式组这些 WebGL2 不存在的能力——它们只应被能力门控挡在运行期之外，
// 真被调用说明门控逻辑有 bug，必须大声失败而不是静默造假。
inline void unsupported_call(const char* fn) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        GLOG_ERROR("WebGL2 shim: {} is not available on WebGL2 "
                   "(capability gating failed?)", fn);
    }
}

} // namespace webgl_detail
} // namespace gryce_engine::render

// ---------------------------------------------------------------------------
// 缺失的 ES3.0 枚举（桌面值，仅在死代码路径被引用）
// ---------------------------------------------------------------------------
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
#ifndef GL_COMMAND_BARRIER_BIT
#define GL_COMMAND_BARRIER_BIT 0x00000040
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#endif

// compute shader 阶段枚举（GLShader::link_compute_program 的 glCreateShader
// 实参）。WebGL2 无 compute，正常被 supports_compute_shaders() 挡在运行期之外。
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif

// 采样器 uniform 类型枚举：GLShader::scan_sampler_uniforms() 的
// is_sampler_type() switch 标签。glGetActiveUniform 在 WebGL2 上永远不会
// 返回这些类型（不存在 1D/3D/multisample/buffer 采样器），纯占位。
#ifndef GL_SAMPLER_1D
#define GL_SAMPLER_1D 0x8B5D
#endif
#ifndef GL_SAMPLER_3D
#define GL_SAMPLER_3D 0x8B5F
#endif
#ifndef GL_SAMPLER_1D_SHADOW
#define GL_SAMPLER_1D_SHADOW 0x8B61
#endif
#ifndef GL_SAMPLER_1D_ARRAY
#define GL_SAMPLER_1D_ARRAY 0x8DC0
#endif
#ifndef GL_SAMPLER_1D_ARRAY_SHADOW
#define GL_SAMPLER_1D_ARRAY_SHADOW 0x8DC3
#endif
#ifndef GL_SAMPLER_2D_MULTISAMPLE
#define GL_SAMPLER_2D_MULTISAMPLE 0x9108
#endif
#ifndef GL_SAMPLER_2D_MULTISAMPLE_ARRAY
#define GL_SAMPLER_2D_MULTISAMPLE_ARRAY 0x910B
#endif
#ifndef GL_SAMPLER_CUBE_MAP_ARRAY
#define GL_SAMPLER_CUBE_MAP_ARRAY 0x900C
#endif
#ifndef GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW
#define GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW 0x900D
#endif
#ifndef GL_SAMPLER_BUFFER
#define GL_SAMPLER_BUFFER 0x8DC2
#endif
#ifndef GL_INT_SAMPLER_1D
#define GL_INT_SAMPLER_1D 0x8DC9
#endif
#ifndef GL_INT_SAMPLER_2D
#define GL_INT_SAMPLER_2D 0x8DCA
#endif
#ifndef GL_INT_SAMPLER_3D
#define GL_INT_SAMPLER_3D 0x8DCB
#endif
#ifndef GL_INT_SAMPLER_CUBE
#define GL_INT_SAMPLER_CUBE 0x8DCC
#endif
#ifndef GL_INT_SAMPLER_2D_ARRAY
#define GL_INT_SAMPLER_2D_ARRAY 0x8DCF
#endif
#ifndef GL_INT_SAMPLER_BUFFER
#define GL_INT_SAMPLER_BUFFER 0x8DD0
#endif
#ifndef GL_UNSIGNED_INT_SAMPLER_1D
#define GL_UNSIGNED_INT_SAMPLER_1D 0x8DD1
#endif
#ifndef GL_UNSIGNED_INT_SAMPLER_2D
#define GL_UNSIGNED_INT_SAMPLER_2D 0x8DD2
#endif
#ifndef GL_UNSIGNED_INT_SAMPLER_3D
#define GL_UNSIGNED_INT_SAMPLER_3D 0x8DD3
#endif
#ifndef GL_UNSIGNED_INT_SAMPLER_CUBE
#define GL_UNSIGNED_INT_SAMPLER_CUBE 0x8DD4
#endif
#ifndef GL_UNSIGNED_INT_SAMPLER_2D_ARRAY
#define GL_UNSIGNED_INT_SAMPLER_2D_ARRAY 0x8DD7
#endif
#ifndef GL_UNSIGNED_INT_SAMPLER_BUFFER
#define GL_UNSIGNED_INT_SAMPLER_BUFFER 0x8DD8
#endif

// 内存屏障位：GLBackend::dispatch_compute 的两次 glMemoryBarrier 实参。
// WebGL2 无 compute，该函数被 supports_compute_shaders() 挡在运行期之外。
#ifndef GL_BUFFER_UPDATE_BARRIER_BIT
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#endif

// gl_utils.h 的 gl_error_string() switch 标签：GL_STACK_OVERFLOW /
// GL_STACK_UNDERFLOW / GL_CONTEXT_LOST 在桌面 gl.h 里，GLES3 头没有。
// GL_CHECK_ERROR 的错误串查表是**活代码**（ES3 也会产这些错误码）。
#ifndef GL_STACK_OVERFLOW
#define GL_STACK_OVERFLOW 0x0503
#endif
#ifndef GL_STACK_UNDERFLOW
#define GL_STACK_UNDERFLOW 0x0504
#endif
#ifndef GL_CONTEXT_LOST
#define GL_CONTEXT_LOST 0x0507
#endif

// GL_DEBUG_OUTPUT 相关（GL 4.3，ES3.0 核心不含）：severity 枚举只出现在
// gl_utils.h 的 debug 回调函数体里，注册函数本身已 !defined(__EMSCRIPTEN__)
// 整块跳过，故这些枚举纯占位。
#ifndef GL_DEBUG_SEVERITY_HIGH
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#endif
#ifndef GL_DEBUG_SEVERITY_MEDIUM
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#endif
#ifndef GL_DEBUG_SEVERITY_LOW
#define GL_DEBUG_SEVERITY_LOW 0x9148
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#endif

// GLShader::to_gl_shader_stage 的 switch 标签。WebGL2 无 geometry shader，
// 引擎也不产该 stage（shader_source_resolver 无 .geom 路径），纯占位。
#ifndef GL_GEOMETRY_SHADER
#define GL_GEOMETRY_SHADER 0x8DD9
#endif

// GLShader::storage_block_binding 的 program interface 枚举（GL 4.3）。
// 配合被 stub 的 glGetProgramResourceIndex 使用；SSBO 在 WebGL2 不存在，
// 该函数被 supports_compute_shaders() 挡在运行期之外，纯占位。
#ifndef GL_SHADER_STORAGE_BLOCK
#define GL_SHADER_STORAGE_BLOCK 0x92E6
#endif
#ifndef GL_BUFFER_BINDING
#define GL_BUFFER_BINDING 0x9302
#endif

// 边框 clamp：WebGL2 不支持 GL_CLAMP_TO_BORDER / GL_TEXTURE_BORDER_COLOR
// （ES3.0 核心不含，浏览器也未暴露 EXT_texture_border_clamp）。
// 见下方 tex_parameteri/sampler_parameteri 的翻译逻辑。
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif

// ---------------------------------------------------------------------------
// GLEW 兼容表面
// ---------------------------------------------------------------------------
#define GLEW_OK 0
// WebGL2 = GLES3：没有 3.3 的 GL_TIME_ELAPSED 查询、没有 4.3 的 compute，
// 也没有 ARB_direct_state_access。恒 0 让 GL 后端的能力探测自动降级。
#define GLEW_VERSION_3_3 0
#define GLEW_VERSION_4_3 0
#define GLEW_ARB_direct_state_access 0
// 各向异性过滤：Emscripten 的 GL 库不提供 GLEW 式宏，改为运行期扩展探测
//（gl_utils.h 的 gl_apply_anisotropy 直接 if (GLEW_EXT_...) 判断）。
#define GLEW_EXT_texture_filter_anisotropic \
    ::gryce_engine::render::webgl_detail::ext_anisotropic()

inline int glewInit() { return GLEW_OK; }
inline const char* glewGetErrorString(int error) {
    (void)error;
    return "no error (GLEW shim: WebGL2 context is current)";
}

// ---------------------------------------------------------------------------
// DSA 入口的 bind-based 实现（真实现，WebGL2 上全部可达）
// ---------------------------------------------------------------------------

// glCreateTextures(target, n, textures) -> glGenTextures
inline void gryce_webgl_create_textures(GLenum target, GLsizei n, GLuint* textures) {
    glGenTextures(n, textures);
    for (GLsizei i = 0; i < n; ++i) {
        gryce_engine::render::webgl_detail::texture_targets()[textures[i]] = target;
    }
}

inline void gryce_webgl_create_framebuffers(GLsizei n, GLuint* ids) {
    glGenFramebuffers(n, ids);
}

inline void gryce_webgl_create_buffers(GLsizei n, GLuint* ids) {
    glGenBuffers(n, ids);
}

inline void gryce_webgl_create_vertex_arrays(GLsizei n, GLuint* ids) {
    glGenVertexArrays(n, ids);
}

// glDeleteTextures：同步失效注册表，避免 id 复用后 target 张冠李戴
inline void gryce_webgl_delete_textures(GLsizei n, const GLuint* textures) {
    for (GLsizei i = 0; i < n; ++i) {
        gryce_engine::render::webgl_detail::texture_targets().erase(textures[i]);
    }
    glDeleteTextures(n, textures);
}

// glBindTexture：登记 target（非 DSA 创建路径的信息来源），再真实绑定。
// 热路径：同一 texture 重复绑同一 target 时跳过哈希写。
inline void gryce_webgl_bind_texture(GLenum target, GLuint texture) {
    if (texture != 0) {
        auto& targets = gryce_engine::render::webgl_detail::texture_targets();
        auto it = targets.find(texture);
        if (it == targets.end()) {
            targets.emplace(texture, target);
        } else {
            it->second = target;
        }
    }
    glBindTexture(target, texture);
}

// 以下三个函数的签名与桌面 GL 的 DSA 入口**逐字一致**（不携带 target）：
// target 由 texture_targets() 注册表查出 —— glCreateTextures / glBindTexture
// 两条创建路径都会登记。注册表缺失时退回 GL_TEXTURE_2D（与 desktop 的
// "未创建纹理"行为一致，会由后续 glGetError 报错）。
inline void gryce_webgl_texture_storage2d(GLuint texture, GLsizei levels,
                                          GLenum internalformat,
                                          GLsizei width, GLsizei height) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glTexStorage2D(target, levels, internalformat, width, height);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

inline void gryce_webgl_texture_sub_image2d(GLuint texture, GLint level,
                                            GLint xoffset, GLint yoffset,
                                            GLsizei width, GLsizei height,
                                            GLenum format, GLenum type,
                                            const void* pixels) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

inline void gryce_webgl_texture_sub_image3d(GLuint texture, GLint level,
                                            GLint xoffset, GLint yoffset, GLint zoffset,
                                            GLsizei width, GLsizei height, GLsizei depth,
                                            GLenum format, GLenum type,
                                            const void* pixels) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth,
                    format, type, pixels);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

inline void gryce_webgl_texture_parameteri(GLuint texture, GLenum pname, GLint param) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glTexParameteri(target, pname, param);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

inline void gryce_webgl_texture_parameterf(GLuint texture, GLenum pname, GLfloat param) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glTexParameterf(target, pname, param);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

inline void gryce_webgl_texture_parameterfv(GLuint texture, GLenum pname,
                                            const GLfloat* param) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glTexParameterfv(target, pname, param);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

inline void gryce_webgl_generate_texture_mipmap(GLuint texture) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glGenerateMipmap(target);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

// glBindTextureUnit：**未门控**的 DSA 入口（Renderer2D 默认贴图绑定）。
inline void gryce_webgl_bind_texture_unit(GLuint unit, GLuint texture) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(target, texture);
}

inline void gryce_webgl_named_framebuffer_texture(GLuint framebuffer, GLenum attachment,
                                                  GLuint texture, GLint level) {
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    // 用注册表里的 target（天空盒等 cubemap 附件也能正确附加）
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
                           gryce_engine::render::webgl_detail::texture_target_of(texture),
                           texture, level);
    if (static_cast<GLuint>(prev) != framebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
    }
}

inline GLenum gryce_webgl_check_named_framebuffer_status(GLuint framebuffer, GLenum target) {
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(target, framebuffer);
    const GLenum status = glCheckFramebufferStatus(target);
    if (static_cast<GLuint>(prev) != framebuffer) {
        glBindFramebuffer(target, static_cast<GLuint>(prev));
    }
    return status;
}

inline void gryce_webgl_enable_vertex_array_attrib(GLuint vao, GLuint index) {
    GLint prev = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(index);
    if (static_cast<GLuint>(prev) != vao) glBindVertexArray(static_cast<GLuint>(prev));
}

inline void gryce_webgl_disable_vertex_array_attrib(GLuint vao, GLuint index) {
    GLint prev = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev);
    glBindVertexArray(vao);
    glDisableVertexAttribArray(index);
    if (static_cast<GLuint>(prev) != vao) glBindVertexArray(static_cast<GLuint>(prev));
}

// ---------------------------------------------------------------------------
// WebGL2 不存在的能力：loud-error 占位（见文件头说明）。
// 这些入口在 ES3.0 头文件里不存在，必须能编译；运行期由
// RenderBackendCapabilities 声明 false 走 CPU 回退，不应被调用。
// ---------------------------------------------------------------------------

// buffer 目标查询：glNamedBufferData/SubData 不携带 target。
// 经 glBindBuffer 包装登记的 buffer -> target 映射反查。
// （GL 后端对它们的调用全部在 gl_dsa_available() 门控内，恒不执行。）
inline void gryce_webgl_named_buffer_data(GLuint buffer, GLsizeiptr size,
                                          const void* data, GLenum usage) {
    gryce_engine::render::webgl_detail::unsupported_call("glNamedBufferData");
    (void)buffer; (void)size; (void)data; (void)usage;
}

inline void gryce_webgl_named_buffer_sub_data(GLuint buffer, GLintptr offset,
                                              GLsizeiptr size, const void* data) {
    gryce_engine::render::webgl_detail::unsupported_call("glNamedBufferSubData");
    (void)buffer; (void)offset; (void)size; (void)data;
}

// VAO 分离属性格式组（ES3.1 / GL 4.3）：WebGL2 无等价物。GL 后端对它们的
// 调用全部在 gl_dsa_available() 门控内（gl_buffer.cpp 有完整的非 DSA
// 备选路径），恒不执行。
inline void gryce_webgl_vertex_array_vertex_buffer(GLuint vao, GLuint bindingindex,
                                                   GLuint buffer, GLintptr offset,
                                                   GLsizei stride) {
    gryce_engine::render::webgl_detail::unsupported_call("glVertexArrayVertexBuffer");
    (void)vao; (void)bindingindex; (void)buffer; (void)offset; (void)stride;
}
inline void gryce_webgl_vertex_array_attrib_format(GLuint vao, GLuint attribindex,
                                                   GLint size, GLenum type,
                                                   GLboolean normalized,
                                                   GLuint relativeoffset) {
    gryce_engine::render::webgl_detail::unsupported_call("glVertexArrayAttribFormat");
    (void)vao; (void)attribindex; (void)size; (void)type; (void)normalized; (void)relativeoffset;
}
inline void gryce_webgl_vertex_array_attrib_iformat(GLuint vao, GLuint attribindex,
                                                    GLint size, GLenum type,
                                                    GLuint relativeoffset) {
    gryce_engine::render::webgl_detail::unsupported_call("glVertexArrayAttribIFormat");
    (void)vao; (void)attribindex; (void)size; (void)type; (void)relativeoffset;
}
inline void gryce_webgl_vertex_array_attrib_binding(GLuint vao, GLuint attribindex,
                                                    GLuint bindingindex) {
    gryce_engine::render::webgl_detail::unsupported_call("glVertexArrayAttribBinding");
    (void)vao; (void)attribindex; (void)bindingindex;
}
inline void gryce_webgl_vertex_array_element_buffer(GLuint vao, GLuint buffer) {
    gryce_engine::render::webgl_detail::unsupported_call("glVertexArrayElementBuffer");
    (void)vao; (void)buffer;
}
inline void gryce_webgl_vertex_array_binding_divisor(GLuint vao, GLuint bindingindex,
                                                     GLuint divisor) {
    gryce_engine::render::webgl_detail::unsupported_call("glVertexArrayBindingDivisor");
    (void)vao; (void)bindingindex; (void)divisor;
}

// compute shader（GPU 剔除 / 实例流）：WebGL2 无。supports_compute=false
// 时管线走 CPU 回退，dispatch 永不发生。
inline void gryce_webgl_dispatch_compute(GLuint num_groups_x, GLuint num_groups_y,
                                         GLuint num_groups_z) {
    gryce_engine::render::webgl_detail::unsupported_call("glDispatchCompute");
    (void)num_groups_x; (void)num_groups_y; (void)num_groups_z;
}

// 间接绘制：WebGL2 无。引擎默认 CPU 提交路径。
inline void gryce_webgl_draw_elements_indirect(GLenum mode, GLenum type,
                                               const void* indirect) {
    gryce_engine::render::webgl_detail::unsupported_call("glDrawElementsIndirect");
    (void)mode; (void)type; (void)indirect;
}
inline void gryce_webgl_draw_arrays_indirect(GLenum mode, const void* indirect) {
    gryce_engine::render::webgl_detail::unsupported_call("glDrawArraysIndirect");
    (void)mode; (void)indirect;
}

// SSBO 程序接口查询（ES3.1 / GL 4.3）：WebGL2 无 SSBO。
inline GLuint gryce_webgl_get_program_resource_index(GLuint program,
                                                     GLenum program_interface,
                                                     const char* name) {
    gryce_engine::render::webgl_detail::unsupported_call("glGetProgramResourceIndex");
    (void)program; (void)program_interface; (void)name;
    return GL_INVALID_INDEX;
}

inline void gryce_webgl_get_program_resourceiv(GLuint program, GLenum program_interface,
                                               GLuint index, GLsizei prop_count,
                                               const GLenum* props, GLsizei buf_size,
                                               GLsizei* length, GLint* params) {
    gryce_engine::render::webgl_detail::unsupported_call("glGetProgramResourceiv");
    (void)program; (void)program_interface; (void)index; (void)prop_count;
    (void)props; (void)buf_size;
    if (length) *length = 0;
    if (params && buf_size > 0) {
        for (GLsizei i = 0; i < buf_size; ++i) params[i] = 0;
    }
}

// 多样本纹理存储（ES3.1）：MSAA 首版关闭（capabilities 上报
// max_msaa_color_samples=1），纹理创建不会走这条路径。
inline void gryce_webgl_tex_image2d_multisample(GLenum target, GLsizei samples,
                                                GLenum internalformat, GLsizei width,
                                                GLsizei height,
                                                GLboolean fixedsamplelocations) {
    gryce_engine::render::webgl_detail::unsupported_call("glTexImage2DMultisample");
    (void)target; (void)samples; (void)internalformat; (void)width; (void)height;
    (void)fixedsamplelocations;
}
inline void gryce_webgl_texture_storage2d_multisample(GLuint texture, GLsizei samples,
                                                      GLenum internalformat,
                                                      GLsizei width, GLsizei height,
                                                      GLboolean fixedsamplelocations) {
    gryce_engine::render::webgl_detail::unsupported_call("glTextureStorage2DMultisample");
    (void)texture; (void)samples; (void)internalformat; (void)width;
    (void)height; (void)fixedsamplelocations;
}

// ---------------------------------------------------------------------------
// 真实现：WebGL2 缺 glDrawBuffer（单缓冲入口），但有 glDrawBuffers(数组)。
// glDrawBuffer(buf) 与 glDrawBuffers(1, &buf) 语义完全等价 —— 把单张量
// 列表透传即可。Renderer2D::disable_color_writes_for_depth_only_target()
// 的 glDrawBuffer(GL_NONE) 因此得到正确行为（禁全部颜色写，深度预通信用）。
// ---------------------------------------------------------------------------
inline void gryce_webgl_draw_buffer(GLenum buf) {
    glDrawBuffers(1, &buf);
}

// ---------------------------------------------------------------------------
// 能力翻译：边框 clamp -> 边缘 clamp
//
// WebGL2 没有 GL_CLAMP_TO_BORDER，也没有 GL_TEXTURE_BORDER_COLOR。引擎只在
// 深度/阴影贴图上用它们（gl_texture.cpp 的 is_depth 分支），期望"采样越界时
// 返回深度 1.0 = 全亮 = 不投影"。
//
// 这个语义在 WebGL2 上由 shader 侧保证，不需要驱动级 border color：
//   pbr.frag / skinned_pbr.frag / deferred_lighting.frag 的 cascade_shadow()
//   都有 `if (proj.x<0||proj.x>1||proj.y<0||proj.y>1) return 1.0;` 显式早退，
//   另有 5% 边缘 smoothstep 淡出到全亮。PCF 滤波核只有几个 texel 宽
//   （<< 5% 贴图边长），淡出区完全覆盖。
// 因此 WRAP_S/WRAP_T 的 CLAMP_TO_BORDER 直接映射为 CLAMP_TO_EDGE，
// TEXTURE_BORDER_COLOR 调用丢弃（log 一次，避免刷屏）。
//
// 走宏重命名而非改调用点：gl_texture.cpp 有 12 处、DSA/非 DSA 两条分支都要
// 覆盖（WebGL 上 gl_dsa_available()=false，走的是非 DSA 的 glTexParameteri）。
// ---------------------------------------------------------------------------
inline void gryce_webgl_tex_parameteri(GLenum target, GLenum pname, GLint param) {
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) {
        if (static_cast<GLenum>(param) == GL_CLAMP_TO_BORDER) param = GL_CLAMP_TO_EDGE;
    }
    glTexParameteri(target, pname, param);
}

inline void gryce_webgl_tex_parameterfv(GLenum target, GLenum pname, const GLfloat* param) {
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        gryce_engine::render::webgl_detail::unsupported_call(
            "glTexParameterfv(GL_TEXTURE_BORDER_COLOR)");
        return;
    }
    glTexParameterfv(target, pname, param);
}

inline void gryce_webgl_sampler_parameteri(GLuint sampler, GLenum pname, GLint param) {
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) {
        if (static_cast<GLenum>(param) == GL_CLAMP_TO_BORDER) param = GL_CLAMP_TO_EDGE;
    }
    glSamplerParameteri(sampler, pname, param);
}

inline void gryce_webgl_sampler_parameterfv(GLuint sampler, GLenum pname, const GLfloat* param) {
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        gryce_engine::render::webgl_detail::unsupported_call(
            "glSamplerParameterfv(GL_TEXTURE_BORDER_COLOR)");
        return;
    }
    glSamplerParameterfv(sampler, pname, param);
}

// ---------------------------------------------------------------------------
// 真实现：glCompressedTextureSubImage2D（DSA 压缩纹理上传）。
// WebGL2 有 bind-based 的 glCompressedTexSubImage2D，用注册表查 target 后
// 保存/恢复绑定即可，语义与桌面 DSA 版一致。
// ---------------------------------------------------------------------------
inline void gryce_webgl_compressed_texture_sub_image2d(GLuint texture, GLint level,
                                                       GLint xoffset, GLint yoffset,
                                                       GLsizei width, GLsizei height,
                                                       GLenum format, GLsizei size,
                                                       const void* data) {
    const GLenum target = gryce_engine::render::webgl_detail::texture_target_of(texture);
    GLint prev = 0;
    glGetIntegerv(gryce_engine::render::webgl_detail::binding_query_of(target), &prev);
    glBindTexture(target, texture);
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height,
                              format, size, data);
    if (static_cast<GLuint>(prev) != texture) glBindTexture(target, static_cast<GLuint>(prev));
}

// ---------------------------------------------------------------------------
// 死代码占位：WebGL2 完全不存在的能力（timer query 64 位读回 / 内存屏障 /
// 缓冲回读）。都不改变控制流以外的状态，只是打一次大声错误。
// ---------------------------------------------------------------------------

// GLBackend::gpu_profile_frame_boundary 的 GL_QUERY_RESULT 读回。该函数被
// gpu_profiling_enabled_ && gpu_profiling_ready_ 双重门控，而 WebGL2 无
// EXT_disjoint_timer_query_webgl2（探针实测 no）→ ready_ 恒 false。
inline void gryce_webgl_get_query_object_ui64v(GLuint id, GLenum pname, GLuint64* params) {
    gryce_engine::render::webgl_detail::unsupported_call("glGetQueryObjectui64v");
    (void)id; (void)pname;
    if (params) *params = 0;
}

// GLBackend::dispatch_compute 的屏障。WebGL2 无 compute，该函数被
// supports_compute_shaders() 挡在运行期之外。
inline void gryce_webgl_memory_barrier(GLbitfield barriers) {
    gryce_engine::render::webgl_detail::unsupported_call("glMemoryBarrier");
    (void)barriers;
}

// GLStorageBuffer::readback 的回读。WebGL2 无 glGetBufferSubData（只有
// glGetBufferParameter*）；SSBO 本身也不存在，readback 被 ssbo_ 空判挡在
// 运行期之外。dst 清零而非留未初始化内存，避免把堆内容泄进画面。
inline void gryce_webgl_get_buffer_sub_data(GLenum target, GLintptr offset,
                                            GLsizeiptr size, void* data) {
    gryce_engine::render::webgl_detail::unsupported_call("glGetBufferSubData");
    (void)target; (void)offset;
    if (data && size > 0) std::memset(data, 0, static_cast<size_t>(size));
}

// ---------------------------------------------------------------------------
// 宏重命名（必须放在所有 shim 函数定义之后：定义内部调用的是真实 ES3 函数）
// ---------------------------------------------------------------------------
#define glCreateTextures gryce_webgl_create_textures
#define glCreateFramebuffers gryce_webgl_create_framebuffers
#define glCreateBuffers gryce_webgl_create_buffers
#define glCreateVertexArrays gryce_webgl_create_vertex_arrays
#define glDeleteTextures gryce_webgl_delete_textures
#define glBindTexture gryce_webgl_bind_texture
#define glTextureStorage2D gryce_webgl_texture_storage2d
#define glTextureSubImage2D gryce_webgl_texture_sub_image2d
#define glTextureSubImage3D gryce_webgl_texture_sub_image3d
#define glTextureParameteri gryce_webgl_texture_parameteri
#define glTextureParameterf gryce_webgl_texture_parameterf
#define glTextureParameterfv gryce_webgl_texture_parameterfv
#define glGenerateTextureMipmap gryce_webgl_generate_texture_mipmap
#define glBindTextureUnit gryce_webgl_bind_texture_unit
#define glNamedFramebufferTexture gryce_webgl_named_framebuffer_texture
#define glCheckNamedFramebufferStatus gryce_webgl_check_named_framebuffer_status
#define glEnableVertexArrayAttrib gryce_webgl_enable_vertex_array_attrib
#define glDisableVertexArrayAttrib gryce_webgl_disable_vertex_array_attrib
#define glNamedBufferData gryce_webgl_named_buffer_data
#define glNamedBufferSubData gryce_webgl_named_buffer_sub_data
#define glVertexArrayVertexBuffer gryce_webgl_vertex_array_vertex_buffer
#define glVertexArrayAttribFormat gryce_webgl_vertex_array_attrib_format
#define glVertexArrayAttribIFormat gryce_webgl_vertex_array_attrib_iformat
#define glVertexArrayAttribBinding gryce_webgl_vertex_array_attrib_binding
#define glVertexArrayElementBuffer gryce_webgl_vertex_array_element_buffer
#define glVertexArrayBindingDivisor gryce_webgl_vertex_array_binding_divisor
#define glDispatchCompute gryce_webgl_dispatch_compute
#define glDrawElementsIndirect gryce_webgl_draw_elements_indirect
#define glDrawArraysIndirect gryce_webgl_draw_arrays_indirect
#define glGetProgramResourceIndex gryce_webgl_get_program_resource_index
#define glTexImage2DMultisample gryce_webgl_tex_image2d_multisample
#define glTextureStorage2DMultisample gryce_webgl_texture_storage2d_multisample
#define glGetQueryObjectui64v gryce_webgl_get_query_object_ui64v
#define glMemoryBarrier gryce_webgl_memory_barrier
#define glGetBufferSubData gryce_webgl_get_buffer_sub_data
#define glDrawBuffer gryce_webgl_draw_buffer
#define glTexParameteri gryce_webgl_tex_parameteri
#define glTexParameterfv gryce_webgl_tex_parameterfv
#define glSamplerParameteri gryce_webgl_sampler_parameteri
#define glSamplerParameterfv gryce_webgl_sampler_parameterfv
#define glCompressedTextureSubImage2D gryce_webgl_compressed_texture_sub_image2d
#define glGetProgramResourceiv gryce_webgl_get_program_resourceiv

#endif // __EMSCRIPTEN__
#endif // GRYCE_RENDER_OPENGL_GL_WEBGL_SHIM_H
