#include "compressed_image.h"

#include <algorithm>

#include "assets/dds_loader.h"
#include "assets/ktx_loader.h"

namespace gryce_engine::assets {

namespace {

std::string lower_ext(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

bool load_compressed_image(const std::string& path, CompressedImage& out) {
    const std::string ext = lower_ext(path);
    if (ext == ".dds") {
        return DDSLoader::load(path, out);
    }
    if (ext == ".ktx") {
        return KTXLoader::load(path, out);
    }
    return false;
}

} // namespace gryce_engine::assets
