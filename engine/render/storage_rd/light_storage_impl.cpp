#include "render/storage_rd/light_storage_impl.h"
#include "render/render_context.h"
#include "render/buffer.h"
#include "utils/glog/glog_lib.h"

#include <cmath>
#include <cstring>

namespace gryce_engine::render {

LightStorageImpl::LightStorageImpl(RenderContext* ctx)
    : ctx_(ctx) {
}

LightStorageImpl::~LightStorageImpl() {
    if (light_buffer_.is_valid() && ctx_) {
        ctx_->destroy_buffer(light_buffer_);
        light_buffer_ = {};
    }
    lights_.clear();
}

LightRID LightStorageImpl::light_create(LightType type) {
    LightRID rid = next_rid_++;
    LightData ld;
    ld.type = type;
    lights_[rid] = ld;
    buffer_dirty_ = true;
    return rid;
}

void LightStorageImpl::light_free(LightRID rid) {
    lights_.erase(rid);
    buffer_dirty_ = true;
}

void LightStorageImpl::light_set_color(LightRID rid, const math::Vector3f& color) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.color = color;
        buffer_dirty_ = true;
    }
}

void LightStorageImpl::light_set_intensity(LightRID rid, float intensity) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.intensity = intensity;
        buffer_dirty_ = true;
    }
}

void LightStorageImpl::light_set_position(LightRID rid, const math::Vector3f& pos) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.position = pos;
        buffer_dirty_ = true;
    }
}

void LightStorageImpl::light_set_direction(LightRID rid, const math::Vector3f& dir) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.direction = dir;
        buffer_dirty_ = true;
    }
}

void LightStorageImpl::light_set_range(LightRID rid, float range) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.range = range;
        buffer_dirty_ = true;
    }
}

void LightStorageImpl::light_set_spot_angle(LightRID rid, float angle) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.spot_angle = angle;
        buffer_dirty_ = true;
    }
}

void LightStorageImpl::light_set_shadow_enabled(LightRID rid, bool enabled) {
    auto it = lights_.find(rid);
    if (it != lights_.end()) {
        it->second.shadow_enabled = enabled;
        buffer_dirty_ = true;
    }
}

const LightData* LightStorageImpl::get_light_data(LightRID rid) const {
    auto it = lights_.find(rid);
    return it != lights_.end() ? &it->second : nullptr;
}

size_t LightStorageImpl::light_count() const {
    return lights_.size();
}

// ---------------------------------------------------------------------------
// 打包光源数据到 GPU 缓冲区格式
// 每光源 20 个 float（80 字节）:
//   [0..3]   type_padding:  type (0=Dir, 1=Point, 2=Spot), yzw padding
//   [4..7]   position:      xyz, w padding
//   [8..11]  direction:     xyz, w padding
//   [12..15] color_intensity: rgb, intensity in w
//   [16..19] params:        range, cos_outer, cos_inner, spot_softness
// ---------------------------------------------------------------------------
void LightStorageImpl::_pack_lights(std::vector<float>& out_buffer) const {
    out_buffer.resize(k_max_lights * k_floats_per_light, 0.0f);

    int idx = 0;
    for (const auto& [rid, light] : lights_) {
        if (idx >= k_max_lights) break;

        const int base = idx * k_floats_per_light;

        // type
        int type_int = 0;
        switch (light.type) {
            case LightType::Directional: type_int = 0; break;
            case LightType::Point:       type_int = 1; break;
            case LightType::Spot:        type_int = 2; break;
        }
        out_buffer[base + 0] = static_cast<float>(type_int);

        // position
        out_buffer[base + 4] = light.position.x;
        out_buffer[base + 5] = light.position.y;
        out_buffer[base + 6] = light.position.z;

        // direction
        out_buffer[base + 8]  = light.direction.x;
        out_buffer[base + 9]  = light.direction.y;
        out_buffer[base + 10] = light.direction.z;

        // color + intensity
        out_buffer[base + 12] = light.color.x;
        out_buffer[base + 13] = light.color.y;
        out_buffer[base + 14] = light.color.z;
        out_buffer[base + 15] = light.intensity;

        // params: range, cos_outer, cos_inner, spot_softness
        out_buffer[base + 16] = light.range;
        if (light.type == LightType::Spot) {
            float half_outer = math::to_radians(light.spot_angle * 0.5f);
            out_buffer[base + 17] = std::cos(half_outer);
            float inner_angle = light.spot_angle * (1.0f - light.spot_softness);
            float half_inner = math::to_radians(inner_angle * 0.5f);
            out_buffer[base + 18] = std::cos(half_inner);
        } else {
            out_buffer[base + 17] = 1.0f; // cos_outer
            out_buffer[base + 18] = 1.0f; // cos_inner
        }
        out_buffer[base + 19] = light.spot_softness;

        ++idx;
    }
}

// ---------------------------------------------------------------------------
// 每帧调用：更新 GPU SSBO
// 如果光源数据有变化，重新打包并上传到 GPU。
// ---------------------------------------------------------------------------
void LightStorageImpl::update_buffers() {
    if (!buffer_dirty_ && light_buffer_.is_valid()) return;
    if (!ctx_) return;

    // 打包数据
    std::vector<float> packed;
    _pack_lights(packed);

    // 创建或更新 SSBO
    if (!light_buffer_.is_valid()) {
        // 首次创建
        light_buffer_ = ctx_->create_buffer();
        if (!light_buffer_.is_valid()) {
            GLOG_ERROR("LightStorageImpl: failed to create light buffer");
            return;
        }
        IBuffer* buf = ctx_->buffer(light_buffer_);
        if (buf) {
            buf->create(k_ssbo_size, packed.data(), BufferUsage::Dynamic);
        }
    } else {
        // 更新已有缓冲
        IBuffer* buf = ctx_->buffer(light_buffer_);
        if (buf) {
            buf->update(packed.data(), k_ssbo_size, 0);
        }
    }

    buffer_dirty_ = false;
}

// ---------------------------------------------------------------------------
// 绑定 SSBO 到指定 binding point
// 在渲染前调用，使 shader 可通过 SSBO 读取光源数据。
// ---------------------------------------------------------------------------
void LightStorageImpl::bind_light_buffer(uint32_t binding_point) {
    if (!light_buffer_.is_valid() || !ctx_) return;
    IBuffer* buf = ctx_->buffer(light_buffer_);
    if (buf) {
        buf->bind(binding_point);
    }
}

} // namespace gryce_engine::render