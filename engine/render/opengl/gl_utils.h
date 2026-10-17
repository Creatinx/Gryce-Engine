// gl_utils.h — OpenGL 后端公共工具：GL 错误码转字符串与错误检查宏（仅调试
// 构建生效）、RenderDoc / Nsight 的调试组标记、GL 调试输出回调，以及纹理
// mip 链 / 各向异性过滤等 GL 辅助函数。
#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GL 错误码转字符串
// ---------------------------------------------------------------------------
inline const char* gl_error_string(GLenum err) {
    switch (err) {
        case GL_NO_ERROR:                      return "GL_NO_ERROR";
        case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
        case GL_STACK_OVERFLOW:                return "GL_STACK_OVERFLOW";
        case GL_STACK_UNDERFLOW:               return "GL_STACK_UNDERFLOW";
        case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_CONTEXT_LOST:                  return "GL_CONTEXT_LOST";
        default:                               return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// GL 错误检查宏：仅记录并清除错误，不再 assert，避免把残留错误变成崩溃。
// 注意：OpenGL 错误会在发生错误的调用之后才通过 glGetError 暴露，因此本条
// 日志的 file/line 只是“发现错误的位置”，真正的错误源在之前的 GL 调用。
// Release 构建下完全剔除：glGetError 是 CPU/GPU 同步点，热路径每帧可达万次。
// ---------------------------------------------------------------------------
#ifndef NDEBUG
#define GL_CHECK_ERROR() \
    do { \
        GLenum err = glGetError(); \
        if (err != GL_NO_ERROR) { \
            GLOG_ERROR("OpenGL error {} ({}) at {}:{}", err, gl_error_string(err), __FILE__, __LINE__); \
        } \
    } while (0)
#else
#define GL_CHECK_ERROR() ((void)0)
#endif

// ---------------------------------------------------------------------------
// GL Debug Group：在 RenderDoc / Nsight 中标记渲染范围
// ---------------------------------------------------------------------------
class GLPushDebugGroup {
public:
    GLPushDebugGroup(const char* name) {
#if defined(_DEBUG) || defined(DEBUG)
        if (glPushDebugGroup) {
            glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
        }
#endif
    }
    ~GLPushDebugGroup() {
#if defined(_DEBUG) || DEBUG
        if (glPopDebugGroup) {
            glPopDebugGroup();
        }
#endif
    }
};

// ---------------------------------------------------------------------------
// GL 调试回调（仅 Debug 构建）
// ---------------------------------------------------------------------------
inline void gl_debug_callback(GLenum source, GLenum type, GLuint id,
                               GLenum severity, GLsizei length,
                               const GLchar* message, const void* user_param) {
    (void)source; (void)type; (void)id; (void)length; (void)user_param;

    // 同一 GL 调试消息 2 秒内只记录一次，避免逐帧刷屏淹没编辑器 Console。
    // 每个唯一消息仍会首次输出；恢复真实错误时再重新记录。
    {
        static std::mutex s_mutex;
        static std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> s_recent;
        static constexpr auto kDedupeWindow = std::chrono::seconds(2);
        std::lock_guard<std::mutex> lock(s_mutex);
        const auto now = std::chrono::steady_clock::now();
        auto it = std::find_if(s_recent.begin(), s_recent.end(),
                               [&](const auto& p) { return p.first == message; });
        if (it != s_recent.end()) {
            if (now - it->second < kDedupeWindow) return;
            it->second = now;
        } else {
            s_recent.emplace_back(message, now);
            if (s_recent.size() > 24) s_recent.erase(s_recent.begin());
        }
    }

    if (severity == GL_DEBUG_SEVERITY_HIGH) {
        GLOG_ERROR("GL DEBUG: {}", message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        GLOG_WARN("GL DEBUG: {}", message);
    } else {
        GLOG_DEBUG("GL DEBUG: {}", message);
    }
}

inline void gl_register_debug_callback() {
// WebGL2 没有 GL_DEBUG_OUTPUT / glDebugMessageCallback（ES3.0 核心不含），
// 整块跳过；Emscripten 的 GL 错误检查（GL_CHECK_ERROR）仍然有效。
#if (defined(_DEBUG) || defined(DEBUG)) && !defined(__EMSCRIPTEN__)
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(gl_debug_callback, nullptr);
    GLOG_INFO("OpenGL debug callback registered");
#endif
}

// ---------------------------------------------------------------------------
// DSA (Direct State Access) 支持检测
// ---------------------------------------------------------------------------
inline bool gl_dsa_available() {
    // OpenGL 4.5+ core 包含 DSA；通过 GLEW 检查扩展或版本。
    static bool checked = false;
    static bool available = false;
    if (!checked) {
        checked = true;
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (version) {
            int major = 0, minor = 0;
            if (std::sscanf(version, "%d.%d", &major, &minor) == 2) {
                available = (major > 4) || (major == 4 && minor >= 5);
            }
        }
        if (!available) {
            available = GLEW_ARB_direct_state_access != 0;
        }
    }
    return available;
}

// ---------------------------------------------------------------------------
// 完整 mip 链级数 = floor(log2(max(w,h))) + 1
//
// glTextureStorage2D 建立的是「不可变存储」：levels 参数决定此后能存在多少级
// mip。只传 1 的话，后面再调 glGenerateTextureMipmap 也无处可写 —— 纹理只有
// level 0，而 MIN_FILTER 又是 GL_LINEAR_MIPMAP_LINEAR，纹理处于不完整状态，
// 采样结果只能由驱动兜底。必须带上完整级数。
// ---------------------------------------------------------------------------
inline int gl_full_mip_levels(int width, int height) {
    int dim = std::max(width, height);
    if (dim <= 0) return 1;
    int levels = 1;
    while (dim > 1) {
        dim >>= 1;
        ++levels;
    }
    return levels;
}

// ---------------------------------------------------------------------------
// 各向异性过滤（GL_EXT_texture_filter_anisotropic）
//
// Vulkan 后端对所有非深度纹理的采样器都开了 maxAnisotropy = 设备上限
// （vk_texture.cpp 的多处 create sampler），GL 后端此前完全没设 —— 同一个
// 场景在斜视地面、大面积 UV 拉伸处 GL 会明显更糊。
//
// 扩展与上限只探测一次；扩展缺失时静默跳过（不影响任何既有行为）。
// ---------------------------------------------------------------------------
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

// target 仅在非 DSA 路径使用（DSA 用 glTextureParameterf 直接改纹理状态）。
inline void gl_apply_anisotropy(GLuint texture_id, GLenum target, bool dsa) {
    static bool checked = false;
    static float max_aniso = 1.0f;
    if (!checked) {
        checked = true;
        if (GLEW_EXT_texture_filter_anisotropic) {
            GLfloat value = 1.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &value);
            if (value > 1.0f) max_aniso = value;
        }
    }
    if (max_aniso <= 1.0f) return;
    if (dsa) {
        glTextureParameterf(texture_id, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_aniso);
    } else {
        glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_aniso);
    }
}

} // namespace gryce_engine::render
