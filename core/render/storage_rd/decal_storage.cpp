#include "render/storage_rd/decal_storage.h"
#include "render/render_context.h"

namespace gryce_engine::render {

void DecalStorage::init(RenderContext* ctx) {
    ctx_ = ctx;
    decals_.clear();
    decals_.reserve(k_max_decals);
}

void DecalStorage::destroy() {
    decals_.clear();
}

int DecalStorage::add_decal(const DecalData& decal) {
    if (static_cast<int>(decals_.size()) >= k_max_decals) return -1;
    decals_.push_back(decal);
    return static_cast<int>(decals_.size()) - 1;
}

void DecalStorage::remove_decal(int index) {
    if (index >= 0 && index < static_cast<int>(decals_.size())) {
        decals_.erase(decals_.begin() + index);
    }
}

void DecalStorage::update_decal(int index, const DecalData& decal) {
    if (index >= 0 && index < static_cast<int>(decals_.size())) {
        decals_[index] = decal;
    }
}

void DecalStorage::clear() {
    decals_.clear();
}

} // namespace gryce_engine::render