#pragma once

#include <cassert>
#include <cstdio>

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
// ---------------------------------------------------------------------------
#define GL_CHECK_ERROR() \
    do { \
        GLenum err = glGetError(); \
        if (err != GL_NO_ERROR) { \
            GLOG_ERROR("OpenGL error {} ({}) at {}:{}", err, gl_error_string(err), __FILE__, __LINE__); \
        } \
    } while (0)

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
    if (severity == GL_DEBUG_SEVERITY_HIGH) {
        GLOG_ERROR("GL DEBUG: {}", message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        GLOG_WARN("GL DEBUG: {}", message);
    } else {
        GLOG_DEBUG("GL DEBUG: {}", message);
    }
}

inline void gl_register_debug_callback() {
#if defined(_DEBUG) || defined(DEBUG)
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

} // namespace gryce_engine::render