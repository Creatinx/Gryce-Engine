#pragma once

// 当任意一个 DLL 模块在编译时，GRYCE_API 视为导出；否则视为导入。
// 各模块定义自己的 BUILDING 宏：
//   GryceCore     -> GRYCE_CORE_BUILDING
//   GryceRenderer -> GRYCE_RENDERER_BUILDING
//   GrycePlatform -> GRYCE_PLATFORM_BUILDING
//   GrycePhysics  -> GRYCE_PHYSICS_BUILDING

#ifdef GRYCE_BUILD_SHARED
    #if defined(GRYCE_CORE_BUILDING) || defined(GRYCE_RENDERER_BUILDING) \
        || defined(GRYCE_PLATFORM_BUILDING) || defined(GRYCE_PHYSICS_BUILDING)
        #ifdef _WIN32
            #define GRYCE_API __declspec(dllexport)
        #else
            #define GRYCE_API __attribute__((visibility("default")))
        #endif
    #else
        #ifdef _WIN32
            #define GRYCE_API __declspec(dllimport)
        #else
            #define GRYCE_API
        #endif
    #endif
#else
    #define GRYCE_API
#endif
