#ifndef GRYCE_WEBGL_GLEW_FORWARD_H
#define GRYCE_WEBGL_GLEW_FORWARD_H

// Emscripten 构建专用：把 <GL/glew.h> 重定向到 WebGL2 shim。
//
// 桌面构建根本不把 cmake/web 加进 include path，此文件不可见，
// 引擎 9 处 #include <GL/glew.h> 零改动；Emscripten 构建时经
// target_include_directories(... cmake/web) 优先命中本文件，
// 再转交真正的 shim 实现。
#include "render/opengl/gl_webgl_shim.h"

#endif // GRYCE_WEBGL_GLEW_FORWARD_H
