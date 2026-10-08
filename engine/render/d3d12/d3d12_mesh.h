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

#include "render/mesh.h"

namespace gryce_engine::render::d3d12 {

class D3D12Mesh final : public IMesh {
public:
    D3D12Mesh() = default;
    ~D3D12Mesh() override;

    void set_context(ID3D12Device* device, ID3D12CommandQueue* queue);

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
    bool create_vb_view();
    bool create_ib_view();
    bool create_instance_view();

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;

    ID3D12Resource* vertex_buffer_ = nullptr;
    ID3D12Resource* index_buffer_ = nullptr;
    ID3D12Resource* instance_buffer_ = nullptr;

    D3D12_VERTEX_BUFFER_VIEW vb_view_{};
    D3D12_VERTEX_BUFFER_VIEW instance_view_{};
    D3D12_INDEX_BUFFER_VIEW ib_view_{};

    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    uint32_t instance_count_ = 0;

    VertexLayout layout_{};
    VertexLayout instance_layout_{};
    bool layout_set_ = false;
};

} // namespace gryce_engine::render::d3d12
