#include "d3d12_mesh.h"

#include "d3d12_upload.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

namespace {

// VertexType → DXGI_FORMAT。D3D12 的顶点格式必须与 shader 中的类型宽度一致，
// 且整数属性必须用 *_UINT/SINT 格式（不能用 UNORM/FLOAT，否则 shader 读到位模式）。
DXGI_FORMAT vertex_type_to_format(VertexType type, bool normalized) {
    switch (type) {
        case VertexType::Float:  return DXGI_FORMAT_R32_FLOAT;
        case VertexType::Float2: return DXGI_FORMAT_R32G32_FLOAT;
        case VertexType::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexType::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VertexType::Int:    return DXGI_FORMAT_R32_SINT;
        case VertexType::UInt:   return DXGI_FORMAT_R32_UINT;
        case VertexType::Int4:   return DXGI_FORMAT_R32G32B32A32_SINT;
        case VertexType::UInt4:  return DXGI_FORMAT_R32G32B32A32_UINT;
    }
    (void)normalized;
    return DXGI_FORMAT_R32G32B32_FLOAT;
}

} // namespace

bool D3D12Mesh::location_to_semantic(uint32_t location, const char** out_name,
                                     uint32_t* out_index) {
    // 与引擎 shaders/*.vert 的 location 约定严格对应。
    // 注意：SPIRV-Cross 把 GLSL 的 location 也翻译成这些语义名（POSITION /
    // NORMAL / TANGENT / TEXCOORDn / COLOR / BLENDINDICES / BLENDWEIGHT），
    // 所以这里的命名必须与 SPIRV-Cross 的输出一致，否则 pipeline 创建会失败。
    switch (location) {
        case 0: *out_name = "POSITION";     *out_index = 0; return true;
        case 1: *out_name = "NORMAL";       *out_index = 0; return true;
        case 2: *out_name = "TANGENT";      *out_index = 0; return true;
        case 3: *out_name = "TEXCOORD";     *out_index = 0; return true;
        case 4: *out_name = "COLOR";        *out_index = 0; return true;
        case 5: *out_name = "BLENDINDICES"; *out_index = 0; return true;
        case 6: *out_name = "BLENDWEIGHT";  *out_index = 0; return true;
        // 实例矩阵：mat4 占 4 个 location（5..8 与骨骼冲突，二选一使用）。
        case 7: *out_name = "TEXCOORD";     *out_index = 1; return true;
        case 8: *out_name = "TEXCOORD";     *out_index = 2; return true;
        default: return false;
    }
}

D3D12Mesh::~D3D12Mesh() {
    release();
}

void D3D12Mesh::set_context(ID3D12Device* device, ID3D12CommandQueue* queue,
                            ResourceRetireSink* retire_sink) {
    device_ = device;
    queue_ = queue;
    retire_sink_ = retire_sink;
}

// 缓冲更新（顶点 / 索引 / 实例流共用）。
//
// ★★ 这里**必须**复用容量足够的旧缓冲，绝不能"每次上传都 Release 重建"：
//    D3D12 严禁销毁仍被已提交命令列表引用的资源。实例流是逐帧上传的
//    （粒子系统每帧 upload_instances，且实例数逐帧变化），旧实现每次先
//    Release 再重建 → 销毁了前 1~2 帧的 draw 仍在读的那块缓冲 → 驱动判非法，
//    表现为随机几帧后 DXGI_ERROR_DEVICE_HUNG / 设备移除（实测 Play 模式必挂，
//    静态帧因为压根不更新实例流所以看不出问题）。
//    容量不足时才新建，并把旧资源挂给后端延迟释放（等 GPU 跑完那几帧再 Release）。
bool D3D12Mesh::update_or_create_buffer(ID3D12Resource** buf, uint32_t* capacity,
                                       const void* data, uint32_t size, const char* what) {
    if (!device_ || !queue_ || !buf || !capacity || !data || size == 0) return false;

    if (*buf && *capacity >= size) {
        // 原地更新：staging → 已有资源，不触碰资源生命周期。
        ID3D12Resource* staging = create_upload_buffer(device_, size, data);
        if (!staging) {
            GLOG_ERROR("D3D12Mesh: staging alloc failed for {} ({} bytes)", what, size);
            return false;
        }
        const bool ok = immediate_submit_copy(device_, queue_, *buf, 0, staging, 0, size);
        staging->Release();
        if (!ok) {
            GLOG_ERROR("D3D12Mesh: in-place copy failed for {}", what);
            return false;
        }
        return true;
    }

    // 容量不足：新建 → 旧资源交给后端延迟释放（没有 sink 时只能立即释放，
    // 那只应发生在后端已销毁之后的收尾阶段）。
    ID3D12Resource* next = create_and_upload(data, size, what);
    if (!next) return false;
    if (*buf) {
        if (retire_sink_) {
            retire_sink_->retire_gpu_resource(*buf);
        } else {
            (*buf)->Release();
        }
    }
    *buf = next;
    *capacity = size;
    return true;
}

ID3D12Resource* D3D12Mesh::create_and_upload(const void* data, uint32_t size, const char* what) {
    if (!device_ || !queue_ || !data || size == 0) return nullptr;
    ID3D12Resource* staging = create_upload_buffer(device_, size, data);
    if (!staging) {
        GLOG_ERROR("D3D12Mesh: staging alloc failed for {} ({} bytes)", what, size);
        return nullptr;
    }
    ID3D12Resource* res = create_buffer_resource(device_, size, false);
    if (!res) {
        staging->Release();
        return nullptr;
    }
    if (!immediate_submit_copy(device_, queue_, res, 0, staging, 0, size)) {
        GLOG_ERROR("D3D12Mesh: copy failed for {}", what);
        res->Release();
        staging->Release();
        return nullptr;
    }
    staging->Release();
    return res;
}

void D3D12Mesh::upload_vertices(const void* data, uint32_t size, uint32_t count) {
    vertex_count_ = 0;
    if (!update_or_create_buffer(&vertex_buffer_, &vertex_buffer_bytes_, data, size,
                                 "vertex buffer")) {
        return;
    }
    vertex_count_ = count;
    create_vb_view();
}

void D3D12Mesh::upload_indices(const void* data, uint32_t size, uint32_t count) {
    index_count_ = 0;
    if (!update_or_create_buffer(&index_buffer_, &index_buffer_bytes_, data, size,
                                 "index buffer")) {
        return;
    }
    index_count_ = count;
    create_ib_view();
}

void D3D12Mesh::set_layout(const VertexLayout& layout) {
    layout_ = layout;
    layout_set_ = true;
    create_vb_view();
}

void D3D12Mesh::set_instance_layout(const VertexLayout& layout) {
    instance_layout_ = layout;
    create_instance_view();
}

void D3D12Mesh::upload_instances(const void* data, uint32_t size, uint32_t count) {
    instance_count_ = 0;
    // ★ 逐帧上传的高频路径（粒子系统每帧调用）：容量够就原地更新，
    //   绝不 Release 重建 —— 见 update_or_create_buffer 的说明。
    if (!update_or_create_buffer(&instance_buffer_, &instance_buffer_bytes_, data, size,
                                 "instance buffer")) {
        return;
    }
    instance_count_ = count;
    create_instance_view();
}

bool D3D12Mesh::create_vb_view() {
    if (!vertex_buffer_) return false;
    const uint32_t stride = layout_set_ && layout_.stride > 0 ? layout_.stride : 0;
    vb_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
    vb_view_.StrideInBytes = stride;
    vb_view_.SizeInBytes = vertex_count_ * stride;
    return stride > 0;
}

bool D3D12Mesh::create_ib_view() {
    if (!index_buffer_) return false;
    ib_view_.BufferLocation = index_buffer_->GetGPUVirtualAddress();
    // 引擎的索引统一按 uint32 上传（IMesh 的 upload_indices 不区分索引宽度）。
    ib_view_.Format = DXGI_FORMAT_R32_UINT;
    ib_view_.SizeInBytes = index_count_ * 4u;
    return true;
}

bool D3D12Mesh::create_instance_view() {
    if (!instance_buffer_ || instance_count_ == 0) return false;
    const uint32_t stride = instance_layout_.stride > 0 ? instance_layout_.stride : 0;
    if (stride == 0) return false;
    instance_view_.BufferLocation = instance_buffer_->GetGPUVirtualAddress();
    instance_view_.StrideInBytes = stride;
    instance_view_.SizeInBytes = instance_count_ * stride;
    return true;
}

void D3D12Mesh::bind() const {
    // D3D12 的缓冲绑定是命令列表状态（IASetVertexBuffers 等），需要 cmd。
    // 这里保留抽象层语义：上层通过 draw()/draw_indexed() 时由 pipeline 转交 cmd。
}

void D3D12Mesh::draw() const {
    // 实际绘制由 backend 在持有命令列表时执行（见 D3D12Backend::draw_mesh）。
    // IMesh::draw() 没有 cmd 参数，因此这里不直接发命令。
}

void D3D12Mesh::draw_indexed() const {
    // 同上。
}

void D3D12Mesh::release() {
    if (vertex_buffer_) { vertex_buffer_->Release(); vertex_buffer_ = nullptr; }
    if (index_buffer_)  { index_buffer_->Release();  index_buffer_ = nullptr; }
    if (instance_buffer_) { instance_buffer_->Release(); instance_buffer_ = nullptr; }
    vertex_buffer_bytes_ = 0;
    index_buffer_bytes_ = 0;
    instance_buffer_bytes_ = 0;
    vb_view_ = {};
    ib_view_ = {};
    instance_view_ = {};
    vertex_count_ = 0;
    index_count_ = 0;
    instance_count_ = 0;
    layout_ = {};
    instance_layout_ = {};
    layout_set_ = false;
}

} // namespace gryce_engine::render::d3d12
