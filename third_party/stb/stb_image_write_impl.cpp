// Single translation unit for stb_image_write implementation.
// Include the header with STB_IMAGE_WRITE_IMPLEMENTATION defined to avoid
// duplicate symbols across the project.

// 导出 stbi_write_png 函数（MinGW DLL 需要显式 dllexport）
#ifdef _WIN32
    #define STBI_WRITE_EXPORT __declspec(dllexport)
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
