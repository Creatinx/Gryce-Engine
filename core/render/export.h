#pragma once

// GryceRenderer / GryceCore 导出宏
// GryceCore 和 GryceRenderer 都可能定义这些 render 类型，
// 因此当编译 GryceCore 或 GryceRenderer 时都视为导出。

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