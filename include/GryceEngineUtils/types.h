#pragma once

// GryceEngineUtils::types.h — 基础类型定义
//
// 嵌入式渲染器框架的公共类型层：把引擎内部的跨 API 资源接口
// （IMesh / ITexture / IShader / Material）与 RenderAPI / LightData
// 统一 re-export 到 GryceEngineUtils 命名空间。用户代码只需包含
// <GryceEngineUtils/renderer.h> 即可使用这些类型。

#include <cstdint>

#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/material.h"
#include "render/render.h"
#include "render/storage_rd/light_storage.h"

namespace GryceEngineUtils {

// 渲染后端 API 选择（Vulkan 优先；OpenGL 为兼容后端）
using RenderAPI = gryce_engine::render::RenderAPI;

// 跨 API 资源接口
using IMesh    = gryce_engine::render::IMesh;
using ITexture = gryce_engine::render::ITexture;
using IShader  = gryce_engine::render::IShader;

// 材质：内部为具体类 Material（PBR 工作流），公开层以 IMaterial 名义暴露
using IMaterial = gryce_engine::render::Material;

// 光源数据（Renderer::set_lights / RenderPipeline::set_submit_lights 使用）
using LightData = gryce_engine::render::LightData;

// 内部渲染命名空间保留（RenderPipeline / RenderContext 等高级访问）
namespace render {
    using namespace gryce_engine::render;
}

} // namespace GryceEngineUtils
