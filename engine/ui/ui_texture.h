#pragma once

#include "export.h"

#include <string>

#include "ui/ui_drawlist.h"

namespace gryce_engine::ui {

// ---------------------------------------------------------------------------
// UI 纹理服务
//
// 模型层（控件）只认识"路径"和 TextureRef，不认识 GPU 句柄；解码图片、创建
// 纹理是宿主的事。宿主在初始化时注册一次 loader，控件就能用
//   image.texture_path = "res:/ui/photo.png";
// 这样的声明式写法拿到贴图。缓存按路径做，同一张图只解码/上传一次。
//
// loader 约定：成功时填 out（index/generation/width/height）并返回 true；
// 失败返回 false。它会在主线程、绘制收集期被调用，实现里不要做逐帧重活。
// ---------------------------------------------------------------------------
using UITextureLoadFn = bool (*)(const char* path, TextureRef* out);

GRYCE_API void ui_set_texture_loader(UITextureLoadFn fn);
GRYCE_API UITextureLoadFn ui_texture_loader();

// 取路径对应的纹理。未注册 loader、加载失败或路径为空时返回无效 TextureRef
// （valid() == false），控件应回退到占位色。
GRYCE_API TextureRef ui_texture(const std::string& path);

// 只查缓存、**不触发加载**。控件绘制期必须用这个：绘制发生在渲染器锁内，
// 而加载回调要回头去取渲染器（同一把锁），直接加载会自我死锁。
// 宿主应在初始化阶段（注入界面之前）用 ui_texture() 预热所有路径。
GRYCE_API TextureRef ui_texture_cached(const std::string& path);

GRYCE_API void ui_clear_texture_cache();

} // namespace gryce_engine::ui
