#pragma once

// ---------------------------------------------------------------------------
// GpuScope — 把所在作用域登记为一个 GPU pass 区间（RAII）
//
//   { GpuScope _gpu(backend, "ssao"); render_ssao(ctx); }   // 只统计这一段
//
// 约束：
//   - 区间必须**平铺**，不能嵌套（GL 的 TIME_ELAPSED 与 Vulkan 的成对时间戳
//     都按平铺序列配对，嵌套会让配对错位）；
//   - 提前 return 也安全：析构函数会闭合区间；
//   - 关闭计时（或后端不支持）时开销为一次虚函数调用 + 一次空判断。
// ---------------------------------------------------------------------------

#include "render/render_context.h"

namespace gryce_engine::render {

class GpuScope {
public:
    // 注意：区间标记会排进命令队列（渲染线程按序执行），因此 ctx 必须在本帧
    // 命令流有效期内存在——所有调用点都在同一个 render_scene 调用栈里，安全。
    GpuScope(RenderContext& ctx, const char* name) : ctx_(&ctx) {
        ctx_->gpu_profile_begin(name);
    }
    ~GpuScope() {
        if (ctx_) ctx_->gpu_profile_end();
    }

    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    RenderContext* ctx_ = nullptr;
};

} // namespace gryce_engine::render
