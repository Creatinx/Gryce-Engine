// d3d12_rt_accel.cpp — DXR 加速结构实现（见头文件设计说明）。
#include "d3d12_rt_accel.h"

#include <cstring>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

namespace {

// 创建一段缓冲。UPLOAD 堆用于 CPU 直写（实例/顶点/索引），DEFAULT 堆用于
// AS 结果/scratch（必须 ALLOW_UNORDERED_ACCESS）。
ID3D12Resource* create_buffer(ID3D12Device* dev, uint64_t size, D3D12_HEAP_TYPE heap,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init_state,
                              D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE) {
    if (!dev || size == 0) return nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, heap_flags, &rd, init_state, nullptr,
                                            IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

// 加速结构结果缓冲**只能**创建时就处于 RAYTRACING_ACCELERATION_STRUCTURE 态：
// 该状态不允许作为 ResourceBarrier 的目标（调试层直接报
// "Resources cannot be transitioned into state ...ACCELERATION_STRUCTURE"，
// 且构建时要求目标已是该态）。所以这里固定初态，全程不做状态转换，
// 只靠 UAV 屏障串序（构建完成 → 着色器/实例引用）。
ID3D12Resource* create_as_buffer(ID3D12Device* dev, uint64_t size) {
    return create_buffer(dev, size, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
}

// scratch 没有状态要求，COMMON 即可（也不参与任何屏障）。
ID3D12Resource* create_scratch_buffer(ID3D12Device* dev, uint64_t size) {
    return create_buffer(dev, size, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COMMON);
}

} // namespace

bool D3D12BottomLevelAS::build(ID3D12Device5* device5, ID3D12GraphicsCommandList4* cmd,
                              const RtGeometry& geo) {
    if (!device5 || !cmd || !geo.positions || !geo.indices || geo.vertex_count == 0 ||
        geo.index_count == 0) {
        return false;
    }
    destroy();
    ID3D12Device* dev = nullptr;
    // 由 command list 拿设备：ID3D12GraphicsCommandList 有 GetDevice。
    cmd->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return false;

    // 1) 顶点/索引缓冲：UPLOAD 堆直接写（AS 几何只要求 GPU 可寻址）。
    const uint64_t vb_bytes = static_cast<uint64_t>(geo.vertex_count) * 12ull;
    const uint64_t ib_bytes = static_cast<uint64_t>(geo.index_count) * 4ull;
    vertex_buffer_ = create_buffer(dev, vb_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    index_buffer_ = create_buffer(dev, ib_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                  D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vertex_buffer_ || !index_buffer_) {
        destroy();
        dev->Release();
        return false;
    }
    {
        void* p = nullptr;
        if (FAILED(vertex_buffer_->Map(0, nullptr, &p)) || !p) {
            destroy();
            dev->Release();
            return false;
        }
        std::memcpy(p, geo.positions, static_cast<size_t>(vb_bytes));
        vertex_buffer_->Unmap(0, nullptr);
    }
    {
        void* p = nullptr;
        if (FAILED(index_buffer_->Map(0, nullptr, &p)) || !p) {
            destroy();
            dev->Release();
            return false;
        }
        std::memcpy(p, geo.indices, static_cast<size_t>(ib_bytes));
        index_buffer_->Unmap(0, nullptr);
    }

    // 2) 几何描述 + 预构建信息
    //
    // 描述体放堆上（geom_desc_）：BuildRaytracingAccelerationStructure 的
    // pGeometryDescs 需要活到**命令列表 Close**，用栈上局部变量会在本函数返回后
    // 悬垂 —— 运行时不报错，直到 Close 才返回 E_INVALIDARG，整帧静默作废。
    // 放堆上还额外保证对象被 move 进 BLAS 缓存后地址不变。
    geom_desc_ = std::make_unique<D3D12_RAYTRACING_GEOMETRY_DESC>();
    D3D12_RAYTRACING_GEOMETRY_DESC& geom = *geom_desc_;
    geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = vertex_buffer_->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = 12;
    geom.Triangles.VertexCount = geo.vertex_count;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.IndexBuffer = index_buffer_->GetGPUVirtualAddress();
    geom.Triangles.IndexCount = geo.index_count;
    geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geom.Triangles.Transform3x4 = 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = geom_desc_.get();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) {
        GLOG_WARN("D3D12RT: BLAS prebuild info empty (DXR unsupported?)");
        destroy();
        dev->Release();
        return false;
    }

    // 3) 结果 / scratch
    result_ = create_as_buffer(dev, info.ResultDataMaxSizeInBytes);
    scratch_ = create_scratch_buffer(dev, info.ScratchDataSizeInBytes);
    dev->Release();
    if (!result_ || !scratch_) {
        destroy();
        return false;
    }

    // 4) 录制构建 + UAV 屏障（AS 资源不做状态转换）
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = inputs;
    desc.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress();
    desc.DestAccelerationStructureData = result_->GetGPUVirtualAddress();
    desc.SourceAccelerationStructureData = 0;
    cmd->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = result_;
    cmd->ResourceBarrier(1, &uav);
    return true;
}

void D3D12BottomLevelAS::destroy() {
    geom_desc_.reset();
    if (result_) {
        result_->Release();
        result_ = nullptr;
    }
    if (scratch_) {
        scratch_->Release();
        scratch_ = nullptr;
    }
    if (vertex_buffer_) {
        vertex_buffer_->Release();
        vertex_buffer_ = nullptr;
    }
    if (index_buffer_) {
        index_buffer_->Release();
        index_buffer_ = nullptr;
    }
}

bool D3D12TopLevelAS::ensure_capacity(ID3D12Device5* device5, uint32_t instance_count) {
    if (!device5 || instance_count == 0) return false;
    if (instance_buffer_ && capacity_ >= instance_count) return true;
    ID3D12Device* dev = nullptr;
    device5->QueryInterface(IID_PPV_ARGS(&dev));
    if (!dev) return false;
    if (instance_buffer_) {
        instance_buffer_->Release();
        instance_buffer_ = nullptr;
    }
    const uint32_t cap = instance_count + instance_count / 2 + 8;
    instance_buffer_ = create_buffer(dev, static_cast<uint64_t>(cap) *
                                              sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                     D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    dev->Release();
    if (!instance_buffer_) return false;
    capacity_ = cap;
    return true;
}

bool D3D12TopLevelAS::update_instances(const void* instance_descs, size_t bytes) {
    if (!instance_buffer_ || !instance_descs || bytes == 0) return false;
    void* p = nullptr;
    if (FAILED(instance_buffer_->Map(0, nullptr, &p)) || !p) return false;
    std::memcpy(p, instance_descs, bytes);
    instance_buffer_->Unmap(0, nullptr);
    return true;
}

bool D3D12TopLevelAS::record_build(ID3D12Device5* device5, ID3D12GraphicsCommandList4* cmd,
                                   uint32_t instance_count, bool force_full) {
    if (!device5 || !cmd || !instance_buffer_ || instance_count == 0) return false;
    ID3D12Device* dev = nullptr;
    cmd->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = instance_count;
    inputs.InstanceDescs = instance_buffer_->GetGPUVirtualAddress();
    // 首建带 ALLOW_UPDATE，之后实例拓扑不变即可 PERFORM_UPDATE（refit）。
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    // 先用"整树重建"的口径算尺寸，保证缓冲容量对两种模式都够（refit 不会更大）。
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) {
        dev->Release();
        return false;
    }

    // 容量不足则重建结果/scratch（同样固定 AS 初态，之后不再做状态转换）。
    if (!result_ || result_size_ < info.ResultDataMaxSizeInBytes) {
        if (result_) {
            result_->Release();
            result_ = nullptr;
        }
        result_ = create_as_buffer(dev, info.ResultDataMaxSizeInBytes);
        result_size_ = result_ ? info.ResultDataMaxSizeInBytes : 0;
        built_once_ = false;
    }
    if (!scratch_ || scratch_size_ < info.ScratchDataSizeInBytes) {
        if (scratch_) {
            scratch_->Release();
            scratch_ = nullptr;
        }
        scratch_ = create_scratch_buffer(dev, info.ScratchDataSizeInBytes);
        scratch_size_ = scratch_ ? info.ScratchDataSizeInBytes : 0;
    }
    dev->Release();
    if (!result_ || !scratch_) return false;

    const bool refit = built_once_ && !force_full;
    if (refit) {
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = inputs;
    desc.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress();
    desc.DestAccelerationStructureData = result_->GetGPUVirtualAddress();
    // PERFORM_UPDATE 必须给出源 AS（= 上一棵树的地址），否则调试层报
    // "SourceAccelerationStructureData must be non-null and 256 byte aligned"。
    desc.SourceAccelerationStructureData = refit ? result_->GetGPUVirtualAddress() : 0;
    cmd->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    // 构建 → 着色器读取：UAV 屏障（AS 资源只能靠 UAV 屏障串序）。
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = result_;
    cmd->ResourceBarrier(1, &uav);

    built_once_ = true;
    return true;
}

void D3D12TopLevelAS::destroy() {
    if (result_) {
        result_->Release();
        result_ = nullptr;
    }
    if (scratch_) {
        scratch_->Release();
        scratch_ = nullptr;
    }
    if (instance_buffer_) {
        instance_buffer_->Release();
        instance_buffer_ = nullptr;
    }
    capacity_ = 0;
    result_size_ = 0;
    scratch_size_ = 0;
    built_once_ = false;
}

} // namespace gryce_engine::render::d3d12
