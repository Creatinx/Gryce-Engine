// miniaudio 单头文件实现单元
// 在整个项目中仅此处定义 MINIAUDIO_IMPLEMENTATION
//
// miniaudio 内置解码器只有 WAV/MP3/FLAC；Ogg Vorbis 需要先引入
// stb_vorbis 头（STB_VORBIS_INCLUDE_STB_VORBIS_H 才会启用 MA_HAS_VORBIS 后端），
// 否则所有 .ogg 都会以 MA_INVALID_FILE 失败。
// 注意只能以 STB_VORBIS_HEADER_ONLY 方式包含：stb_vorbis 实现里的
// L/C/R 通道宏会泄漏，破坏 windows.h（winnt.h 位域与 extern "C" 解析失败），
// 因此 stb_vorbis 的实现在 stb_vorbis_impl.cpp 单独编译。
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
