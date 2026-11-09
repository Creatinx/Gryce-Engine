// =============================================================================
// d3d12_mesh — IMesh 的 D3D12 实现（M3.4）
//
// D3D12 与 GL 在顶点输入上的根本差异：
//   * GL 用 glVertexAttribPointer(location, ...) 按 **location** 匹配；
//   * D3D12 用 D3D12_INPUT_ELEMENT_DESC 按 **语义名**（如 "POSITION" /
//     "NORMAL" / "TEXCOORD"）匹配，location 只体现为语义名后的数字下标。
//     即 location=0 → SemanticName="POSITION", SemanticIndex=0。
//   因此后端必须维护 location → (semantic, index) 的映射表，且必须与
//   着色器侧（GLSL → SPIRV-Cross → HLSL 后）实际生成的语义名一致。
//
// 引擎的顶点属性 location 约定（从 shaders/*.vert 归纳，必须保持一致）：
//     0 = aPos        vec3   → POSITION
//     1 = aNormal     vec3   → NORMAL
//     2 = aTangent    vec3   → TANGENT
//     3 = aTexCoord   vec2   → TEXCOORD0
//     4 = aColor      vec3/4 → COLOR
//     5 = aBoneIds    uvec4  → BLENDINDICES（蒙皮）
//     6 = aBoneWeights vec4  → BLENDWEIGHT（蒙皮）
//     5..8 = instance mat4   → INSTANCE_* (TEXCOORD4..7)（实例化，与骨骼冲突，
//            由调用方通过 set_instance_layout 区分，二者不会同时使用）
//
// 顶点/索引缓冲都用 DEFAULT 堆 + 一次性上传（网格数据是静态的）。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>

#include "d3d12_retire.h"
#include "render/mesh.h"

namespace gryce_engine::render::d3d12 {

// ResourceRetireSink 定义在 d3d12_retire.h（D3D12Buffer 也用同一条通道）。
// 触发这条通道的典型场景是**粒子系统**：它每帧调用 upload_instances()，而实例数
// 逐帧变化 —— 旧实现每次上传都先 Release 再重建实例缓冲，于是"销毁 GPU 正在读的
// 缓冲" → 实测在 Play 模式（粒子开始模拟）下随机几帧后直接 DEVICE_HUNG。

class D3D12Mesh final : public IMesh {
public:
    D3D12Mesh() = default;
    ~D3D12Mesh() override;

    void set_context(ID3D12Device* device, ID3D12CommandQueue* queue,
                     ResourceRetireSink* retire_sink = nullptr);

    // ---- IMesh ----
    void upload_vertices(const void* data, uint32_t size, uint32_t count) override;
    void upload_indices(const void* data, uint32_t size, uint32_t count) override;
    void set_layout(const VertexLayout& layout) override;

    void set_instance_layout(const VertexLayout& layout) override;
    void upload_instances(const void* data, uint32_t size, uint32_t count) override;
    uint32_t instance_count() const override { return instance_count_; }

    void bind() const override;
    void draw() const override;
    void draw_indexed() const override;

    uint32_t vertex_count() const override { return vertex_count_; }
    uint32_t index_count() const override { return index_count_; }

    // ---- 光追几何（DXR BLAS 的输入）----
    //
    // DXR 的几何输入必须是 GPU 缓冲，而 BLAS 只吃"紧凑 xyz + uint32 索引"，
    // 顶点流里还夹着法线/切线/UV，所以这里在 CPU 侧解出一份紧凑副本。
    // 只有后端真正启用了 DXR 时才保留原始字节（见 set_ray_tracing_capture），
    // 否则不额外占内存。
    bool get_ray_tracing_geometry(const float*& out_positions, uint32_t& out_vertex_count,
                                  const uint32_t*& out_indices,
                                  uint32_t& out_index_count) const override;
    void set_ray_tracing_capture(bool enabled) { capture_rt_geometry_ = enabled; }
    // 2D UI 会在一帧内多次复用同一个 mesh 句柄提交不同批次。若顶点缓冲
    // 原地覆盖，GPU 真正执行前面 draw 时看到的会是最后一批数据。
    // 开启后每次 upload 都创建独立顶点缓冲，旧缓冲走 retire 通道延迟释放。
    void set_streaming_vertices(bool enabled) { streaming_vertices_ = enabled; }

    // ---- D3D12 侧扩展 ----
    ID3D12Resource* vertex_buffer() const { return vertex_buffer_; }
    ID3D12Resource* index_buffer() const { return index_buffer_; }
    ID3D12Resource* instance_buffer() const { return instance_buffer_; }
    D3D12_VERTEX_BUFFER_VIEW vertex_view() const { return vb_view_; }
    D3D12_VERTEX_BUFFER_VIEW instance_view() const { return instance_view_; }
    D3D12_INDEX_BUFFER_VIEW index_view() const { return ib_view_; }

    // 把 location → 语义名的映射暴露出来，供 pipeline 构建 INPUT_LAYOUT 时使用。
    // 返回 false 表示该 location 无已知语义（调用方应报错而不是静默丢属性）。
    static bool location_to_semantic(uint32_t location, const char** out_name,
                                     uint32_t* out_index);

    // 当前顶点布局（draw 时后端据此建/取 PSO）。
    const VertexLayout& layout() const { return layout_; }
    const VertexLayout& instance_layout() const { return instance_layout_; }
    bool has_layout() const { return layout_set_; }

    bool is_instance_stream_valid() const { return instance_count_ > 0 && instance_buffer_ != nullptr; }
    // 释放并重建缓冲（尺寸变化时由上传路径内部调用）。
    void release();

private:
    ID3D12Resource* create_and_upload(const void* data, uint32_t size, const char* what);
    // 缓冲更新：容量够就**原地更新**（不销毁资源）；不够才新建并把旧的交给
    // 后端延迟释放。返回是否成功。
    bool update_or_create_buffer(ID3D12Resource** buf, uint32_t* capacity, const void* data,
                                 uint32_t size, const char* what);
    bool create_vb_view();
    bool create_ib_view();
    bool create_instance_view();

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ResourceRetireSink* retire_sink_ = nullptr;

    ID3D12Resource* vertex_buffer_ = nullptr;
    ID3D12Resource* index_buffer_ = nullptr;
    ID3D12Resource* instance_buffer_ = nullptr;
    // 已分配的**字节容量**（不是顶点/实例个数）：容量够就原地更新，
    // 避免"每帧重建缓冲"这种既慢又在多帧在飞时致挂的写法。
    uint32_t vertex_buffer_bytes_ = 0;
    uint32_t index_buffer_bytes_ = 0;
    uint32_t instance_buffer_bytes_ = 0;

    D3D12_VERTEX_BUFFER_VIEW vb_view_{};
    D3D12_VERTEX_BUFFER_VIEW instance_view_{};
    D3D12_INDEX_BUFFER_VIEW ib_view_{};

    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    uint32_t instance_count_ = 0;

    VertexLayout layout_{};
    VertexLayout instance_layout_{};
    bool layout_set_ = false;

    // 光追几何：原始顶点/索引字节 + 懒解出的紧凑副本。
    bool capture_rt_geometry_ = false;
    bool streaming_vertices_ = false;
    std::vector<uint8_t> rt_vertex_bytes_;
    std::vector<uint32_t> rt_index_bytes_;
    mutable std::vector<float> rt_positions_;
    mutable std::vector<uint32_t> rt_indices_;
    mutable bool rt_geometry_ready_ = false;
};

} // namespace gryce_engine::render::d3d12
