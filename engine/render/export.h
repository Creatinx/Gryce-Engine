#pragma once

// 先引入 core/export.h，确保 GRYCE_API 在任意包含路径下都有定义。
// （例如 core/math/math.h 被 core/render/ 下的源文件包含时，
//  #include "export.h" 会优先定位到本文件，导致 GRYCE_API 缺失。）
#include "../export.h"

// render 抽象/管线的导出宏。
// 渲染抽象类（RenderContext / RenderPipeline / Render2D / FontAtlas 等）
// 的实现在 GryceCore.dll，头文件已随实现归位到 GryceCore 的公共头集合；
// 后端类（GL/VK 的 Backend/Buffer/Shader 等）实现在 GryceRenderer.dll。
// 两类 render 类型共用本宏：编译任意引擎 DLL 时按导出处理，
// 外部消费者按导入处理（符号实际位于 GryceCore.dll）。

#ifndef GRYCE_RENDERER_API
    #if defined(GRYCE_CORE_BUILDING) || defined(GRYCE_RENDERER_BUILDING) \
        || defined(GRYCE_PLATFORM_BUILDING) || defined(GRYCE_PHYSICS_BUILDING)
        #ifdef _WIN32
            #define GRYCE_RENDERER_API __declspec(dllexport)
        #else
            #define GRYCE_RENDERER_API __attribute__((visibility("default")))
        #endif
    #else
        #ifdef _WIN32
            #define GRYCE_RENDERER_API __declspec(dllimport)
        #else
            #define GRYCE_RENDERER_API
        #endif
    #endif
#endif
