// core/assets/stb_image_impl.cpp
// stb_image 的单文件实现（STB_IMAGE_IMPLEMENTATION 只能在一个 .cpp 中定义，
// 否则会在链接时出现 multiple definition 错误）。

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#pragma GCC diagnostic pop
