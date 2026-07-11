#include "cursor.h"

#include <algorithm>
#include <utility>

#include <GLFW/glfw3.h>
#include <stb_image.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::platform {

Cursor::Cursor() = default;

Cursor::~Cursor() {
    destroy();
}

Cursor::Cursor(Cursor&& other) noexcept
    : cursor_(other.cursor_)
    , pixels_(std::move(other.pixels_))
    , width_(other.width_)
    , height_(other.height_) {
    other.cursor_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
}

Cursor& Cursor::operator=(Cursor&& other) noexcept {
    if (this != &other) {
        destroy();
        cursor_ = other.cursor_;
        pixels_ = std::move(other.pixels_);
        width_ = other.width_;
        height_ = other.height_;
        other.cursor_ = nullptr;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool Cursor::load_from_file(const std::string& path, int hot_x, int hot_y) {
    destroy();

    int w = 0, h = 0, channels = 0;
    // 强制解码成 RGBA，方便统一传给 GLFW
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        GLOG_WARN("Cursor: failed to load image '{}' (stbi_reason: {})", path, stbi_failure_reason());
        return false;
    }

    width_ = w;
    height_ = h;
    pixels_.assign(data, data + static_cast<std::size_t>(w * h * 4));
    stbi_image_free(data);

    GLFWimage image{};
    image.width = width_;
    image.height = height_;
    image.pixels = pixels_.data();

    hot_x = std::clamp(hot_x, 0, width_ - 1);
    hot_y = std::clamp(hot_y, 0, height_ - 1);

    cursor_ = glfwCreateCursor(&image, hot_x, hot_y);
    if (!cursor_) {
        GLOG_ERROR("Cursor: glfwCreateCursor failed for '{}'", path);
        pixels_.clear();
        width_ = 0;
        height_ = 0;
        return false;
    }

    GLOG_INFO("Cursor loaded: {} ({}x{}, hot=({},{}))", path, width_, height_, hot_x, hot_y);
    return true;
}

void Cursor::destroy() {
    if (cursor_) {
        glfwDestroyCursor(cursor_);
        cursor_ = nullptr;
    }
    pixels_.clear();
    width_ = 0;
    height_ = 0;
}

} // namespace gryce_engine::platform
