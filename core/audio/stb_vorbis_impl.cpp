// stb_vorbis 实现单元（Ogg Vorbis 解码器，miniaudio 的 Vorbis 后端依赖它）。
//
// 必须与 miniaudio_impl.cpp 分离编译：stb_vorbis.c 的整数转换段会
// #define L/C/R/PLAYBACK_* 且从不 #undef，若与 miniaudio 引入的
// windows.h 处于同一 TU，会破坏 winnt.h（位域、extern "C"）导致编译失败。
#define STB_VORBIS_IMPLEMENTATION
#include "stb_vorbis.c"
