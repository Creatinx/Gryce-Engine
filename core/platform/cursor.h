#pragma once

#include <string>
#include <vector>

struct GLFWcursor;

namespace gryce_engine::platform {

// ---------------------------------------------------------------------------
// Cursor — 自定义鼠标光标（基于 GLFWcursor）
// ---------------------------------------------------------------------------
class Cursor {
public:
    Cursor();
    ~Cursor();

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    Cursor(Cursor&& other) noexcept;
    Cursor& operator=(Cursor&& other) noexcept;

    // 从图片文件加载光标（支持 PNG/BMP 等 stb_image 能解析的格式）
    // hot_x/hot_y：点击热点，默认左上角 (0,0)
    bool load_from_file(const std::string& path, int hot_x = 0, int hot_y = 0);

    bool is_valid() const { return cursor_ != nullptr; }
    GLFWcursor* native_handle() const { return cursor_; }

    void destroy();

private:
    GLFWcursor* cursor_ = nullptr;
    std::vector<unsigned char> pixels_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace gryce_engine::platform
