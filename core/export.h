#pragma once

#ifdef GRYCE_BUILD_SHARED
    #ifdef GRYCE_CORE_BUILDING
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
