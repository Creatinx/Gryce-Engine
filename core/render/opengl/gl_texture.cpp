#include "gl_texture.h"

#include "gl_utils.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>

// stb_image 声明在 stb_image.h 中，实现集中在 core/assets/stb_image_impl.cpp
#include <stb_image.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

constexpr int k_max_texture_slots = 32;

// 当前已绑定到各 slot 的 2D texture id，避免重复的 glBindTextureUnit/glBindTexture。
thread_local GLuint g_bound_textures[k_max_texture_slots] = {};
// 非 DSA 路径下当前激活的 texture unit。
thread_local int g_active_texture_unit = -1;

void clear_texture_slot_cache(GLuint texture_id) {
    for (int i = 0; i < k_max_texture_slots; ++i) {
        if (g_bound_textures[i] == texture_id) {
            g_bound_textures[i] = 0;
        }
    }
}

} // namespace

// 本地垂直翻转图像，避免修改 stbi_image 全局状态
static void flip_image_vertical(unsigned char* data, int width, int height, int channels) {
    if (height <= 1 || !data) return;
    std::size_t row_size = static_cast<std::size_t>(width * channels);
    std::vector<unsigned char> temp(row_size);
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* top = data + y * width * channels;
        unsigned char* bottom = data + (height - 1 - y) * width * channels;
        std::memcpy(temp.data(), top, row_size);
        std::memcpy(top, bottom, row_size);
        std::memcpy(bottom, temp.data(), row_size);
    }
}

GLTexture::GLTexture() : texture_id_(0), width_(0), height_(0), channels_(4) {}

GLTexture::~GLTexture() {
    if (texture_id_) {
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }
}

namespace {

// 兼容旧版 GLEW / 部分驱动未声明的压缩格式常量
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RED_RGTC1
#define GL_COMPRESSED_RED_RGTC1 0x8DBB
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT
#define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT 0x8E8F
#endif
#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#endif
#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

struct GLFormatInfo {
    GLint internal_format = 0;
    GLenum format = 0;
    GLenum type = GL_UNSIGNED_BYTE;
    int channels = 4;
};

GLFormatInfo to_gl_format(TextureFormat fmt) {
    switch (fmt) {
    case TextureFormat::R8:             return {GL_R8,              GL_RED,  GL_UNSIGNED_BYTE, 1};
    case TextureFormat::RG8:            return {GL_RG8,             GL_RG,   GL_UNSIGNED_BYTE, 2};
    case TextureFormat::RGB8:           return {GL_RGB8,            GL_RGB,  GL_UNSIGNED_BYTE, 3};
    case TextureFormat::RGBA8:          return {GL_RGBA8,           GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case TextureFormat::RGBA16F:        return {GL_RGBA16F,         GL_RGBA, GL_HALF_FLOAT,    4};
    case TextureFormat::RGBA32F:        return {GL_RGBA32F,         GL_RGBA, GL_FLOAT,         4};
    case TextureFormat::Depth16:        return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 1};
    case TextureFormat::Depth24:        return {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,   1};
    case TextureFormat::Depth24Stencil8:return {GL_DEPTH24_STENCIL8,  GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8, 2};
    case TextureFormat::Depth32F:       return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 1};
    // 压缩格式：format/type 字段不会被使用
    case TextureFormat::BC1_RGB:        return {GL_COMPRESSED_RGB_S3TC_DXT1_EXT,   GL_RGB,  GL_UNSIGNED_BYTE, 4};
    case TextureFormat::BC1_RGBA:       return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,  GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case TextureFormat::BC2:            return {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,  GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case TextureFormat::BC3:            return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,  GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case TextureFormat::BC4:            return {GL_COMPRESSED_RED_RGTC1,           GL_RED,  GL_UNSIGNED_BYTE, 1};
    case TextureFormat::BC5:            return {GL_COMPRESSED_RG_RGTC2,            GL_RG,   GL_UNSIGNED_BYTE, 2};
    case TextureFormat::BC6H:           return {GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT, GL_RGB, GL_FLOAT, 3};
    case TextureFormat::BC7:            return {GL_COMPRESSED_RGBA_BPTC_UNORM,     GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case TextureFormat::ETC2_RGB:       return {GL_COMPRESSED_RGB8_ETC2,           GL_RGB,  GL_UNSIGNED_BYTE, 3};
    case TextureFormat::ETC2_RGBA:      return {GL_COMPRESSED_RGBA8_ETC2_EAC,      GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case TextureFormat::ASTC_4x4:       return {GL_COMPRESSED_RGBA_ASTC_4x4_KHR,   GL_RGBA, GL_UNSIGNED_BYTE, 4};
    }
    return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4};
}

} // namespace

bool GLTexture::load_from_file(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 0);
    if (!data) {
        GLOG_ERROR("Failed to load texture: {}", path);
        return false;
    }

    // stb_image 默认 top-down，OpenGL 需要 bottom-up，本地翻转避免全局状态污染
    flip_image_vertical(data, w, h, ch);

    if (texture_id_) {
        // 旧 id 可能仍缓存在 g_bound_textures 槽位中；删除前先失效，
        // 避免驱动复用同一 id 后 bind() 误判"已绑定"而跳过新纹理的绑定。
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }

    width_ = w;
    height_ = h;
    channels_ = ch;
    is_cubemap_ = false;

    const bool dsa = gl_dsa_available();
    if (dsa) {
        glCreateTextures(GL_TEXTURE_2D, 1, &texture_id_);
    } else {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
    }

    GLint internal_format = (ch == 4) ? GL_RGBA : (ch == 3) ? GL_RGB : GL_RED;
    GLenum format = (ch == 4) ? GL_RGBA : (ch == 3) ? GL_RGB : GL_RED;

    if (dsa) {
        glTextureStorage2D(texture_id_, 1, internal_format, w, h);
        glTextureSubImage2D(texture_id_, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, data);
        glGenerateTextureMipmap(texture_id_);

        glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    stbi_image_free(data);

    GLOG_INFO("Texture loaded: {} ({}x{}, {} channels) tex_id={}", path, w, h, ch, texture_id_);
    return true;
}

bool GLTexture::create_empty(int width, int height, int channels) {
    if (texture_id_) {
        // 旧 id 可能仍缓存在 g_bound_textures 槽位中；删除前先失效，
        // 避免驱动复用同一 id 后 bind() 误判"已绑定"而跳过新纹理的绑定。
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }

    width_ = width;
    height_ = height;
    channels_ = channels;
    is_cubemap_ = false;

    const bool dsa = gl_dsa_available();
    if (dsa) {
        glCreateTextures(GL_TEXTURE_2D, 1, &texture_id_);
    } else {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
    }

    GLenum format = (channels == 4) ? GL_RGBA : GL_RGB;

    if (dsa) {
        glTextureStorage2D(texture_id_, 1, format, width, height);
        glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return true;
}

bool GLTexture::upload_data(const void* data, int width, int height, int channels) {
    if (!data) return false;
    if (texture_id_) {
        // 旧 id 可能仍缓存在 g_bound_textures 槽位中；删除前先失效，
        // 避免驱动复用同一 id 后 bind() 误判"已绑定"而跳过新纹理的绑定。
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }

    width_ = width;
    height_ = height;
    channels_ = channels;
    is_cubemap_ = false;

    const bool dsa = gl_dsa_available();
    if (dsa) {
        glCreateTextures(GL_TEXTURE_2D, 1, &texture_id_);
    } else {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
    }

    // FontAtlas 生成 top-down 单通道 R8 位图。OpenGL 中 v=0 对应纹理数据的第 0 行，
    // 因此 top-down 数据直接上传即可与 FontAtlas 的 top-down UV 匹配。
    // 单通道 GL_RED 在某些驱动/上下文组合下会出现采样为 0 的问题，扩展成 RGBA8 更稳定。
    std::vector<unsigned char> upload_buffer;
    const void* upload_data = data;
    // glTextureStorage2D / glTexImage2D 的 internalFormat 必须是 sized format
    GLint internal_format = (channels == 4) ? GL_RGBA8 : (channels == 3) ? GL_RGB8 : GL_RGBA8;
    GLenum format = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RGBA;

    if (channels == 1 && data) {
        upload_buffer.resize(static_cast<std::size_t>(width * height * 4));
        const unsigned char* src = static_cast<const unsigned char*>(data);
        unsigned char* dst = upload_buffer.data();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned char v = src[y * width + x];
                std::size_t dst_idx = static_cast<std::size_t>((y * width + x) * 4);
                dst[dst_idx + 0] = v;
                dst[dst_idx + 1] = v;
                dst[dst_idx + 2] = v;
                dst[dst_idx + 3] = v;
            }
        }
        upload_data = upload_buffer.data();
    } else if (channels == 2 && data) {
        // channels==2（grayscale+alpha）：源缓冲只有 2 字节/像素，
        // 若直接按 GL_RGBA/4 字节读取会越界，必须扩展成 RGBA（R=G=B=gray, A=alpha）。
        upload_buffer.resize(static_cast<std::size_t>(width * height * 4));
        const unsigned char* src = static_cast<const unsigned char*>(data);
        unsigned char* dst = upload_buffer.data();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                std::size_t src_idx = static_cast<std::size_t>((y * width + x) * 2);
                unsigned char gray = src[src_idx + 0];
                unsigned char alpha = src[src_idx + 1];
                std::size_t dst_idx = static_cast<std::size_t>((y * width + x) * 4);
                dst[dst_idx + 0] = gray;
                dst[dst_idx + 1] = gray;
                dst[dst_idx + 2] = gray;
                dst[dst_idx + 3] = alpha;
            }
        }
        upload_data = upload_buffer.data();
    }

    if (dsa) {
        glTextureStorage2D(texture_id_, 1, internal_format, width, height);
        glTextureSubImage2D(texture_id_, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, upload_data);
        glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(texture_id_, GL_TEXTURE_BASE_LEVEL, 0);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAX_LEVEL, 0);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, upload_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    GLOG_INFO("Texture uploaded: {}x{}, {} channels tex_id={}", width, height, channels, texture_id_);
    return true;
}

bool GLTexture::upload_cubemap(const void* faces[6], int width, int height, int channels) {
    if (!faces || width <= 0 || height <= 0) return false;
    for (int i = 0; i < 6; ++i) {
        if (!faces[i]) return false;
    }

    if (texture_id_) {
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }

    width_ = width;
    height_ = height;
    channels_ = channels;
    is_cubemap_ = true;

    GLint internal_format = (channels == 4) ? GL_RGBA8 : (channels == 3) ? GL_RGB8 : GL_R8;
    GLenum format = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;

    const bool dsa = gl_dsa_available();
    if (dsa) {
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &texture_id_);
        glTextureStorage2D(texture_id_, 1, internal_format, width, height);
        for (int i = 0; i < 6; ++i) {
            glTextureSubImage3D(texture_id_, 0, 0, 0, i, width, height, 1,
                                format, GL_UNSIGNED_BYTE, faces[i]);
        }
        glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    } else {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, texture_id_);
        for (int i = 0; i < 6; ++i) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, internal_format,
                         width, height, 0, format, GL_UNSIGNED_BYTE, faces[i]);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }

    GLOG_INFO("Cubemap uploaded: {}x{}, {} channels tex_id={}", width, height, channels, texture_id_);
    return true;
}

bool GLTexture::create_depth(int width, int height) {
    return create(TextureFormat::Depth24, width, height, nullptr);
}

bool GLTexture::create(TextureFormat format, int width, int height, const void* data) {
    if (width <= 0 || height <= 0) return false;

    if (texture_id_) {
        // 旧 id 可能仍缓存在 g_bound_textures 槽位中；删除前先失效，
        // 避免驱动复用同一 id 后 bind() 误判"已绑定"而跳过新纹理的绑定。
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }

    GLFormatInfo info = to_gl_format(format);
    width_ = width;
    height_ = height;
    channels_ = info.channels;
    is_cubemap_ = false;

    const bool dsa = gl_dsa_available();
    if (dsa) {
        glCreateTextures(GL_TEXTURE_2D, 1, &texture_id_);
    } else {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
    }

    bool is_depth = (format == TextureFormat::Depth16 ||
                     format == TextureFormat::Depth24 ||
                     format == TextureFormat::Depth24Stencil8 ||
                     format == TextureFormat::Depth32F);

    if (dsa) {
        glTextureStorage2D(texture_id_, 1, info.internal_format, width, height);
        if (data) {
            glTextureSubImage2D(texture_id_, 0, 0, 0, width, height, info.format, info.type, data);
        }

        if (is_depth) {
            glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
            glTextureParameterfv(texture_id_, GL_TEXTURE_BORDER_COLOR, border_color);
            if (format != TextureFormat::Depth24Stencil8) {
                glTextureParameteri(texture_id_, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                glTextureParameteri(texture_id_, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            }
        } else {
            glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, info.internal_format, width, height, 0, info.format, info.type, data);

        if (is_depth) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
            if (format != TextureFormat::Depth24Stencil8) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            }
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    GLOG_INFO("Texture created: {}x{} format={}", width, height, static_cast<int>(format));
    return true;
}

bool GLTexture::create_compressed(TextureFormat format, int width, int height,
                                  int mip_levels, const void* const* mip_data,
                                  const size_t* mip_sizes) {
    if (width <= 0 || height <= 0 || mip_levels <= 0 || !mip_data || !mip_sizes) return false;
    if (!is_compressed_format(format)) return false;

    if (texture_id_) {
        // 旧 id 可能仍缓存在 g_bound_textures 槽位中；删除前先失效，
        // 避免驱动复用同一 id 后 bind() 误判"已绑定"而跳过新纹理的绑定。
        clear_texture_slot_cache(texture_id_);
        glDeleteTextures(1, &texture_id_);
    }

    const GLFormatInfo info = to_gl_format(format);
    width_ = width;
    height_ = height;
    channels_ = info.channels;
    is_cubemap_ = false;

    const bool dsa = gl_dsa_available();
    if (dsa) {
        glCreateTextures(GL_TEXTURE_2D, 1, &texture_id_);
        glTextureStorage2D(texture_id_, mip_levels, info.internal_format, width, height);
        for (int i = 0; i < mip_levels; ++i) {
            const int mw = std::max(1, width >> i);
            const int mh = std::max(1, height >> i);
            glCompressedTextureSubImage2D(texture_id_, i, 0, 0, mw, mh,
                                          info.internal_format,
                                          static_cast<GLsizei>(mip_sizes[i]), mip_data[i]);
        }
        glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER,
                            mip_levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTextureParameteri(texture_id_, GL_TEXTURE_BASE_LEVEL, 0);
        glTextureParameteri(texture_id_, GL_TEXTURE_MAX_LEVEL, mip_levels - 1);
    } else {
        glGenTextures(1, &texture_id_);
        glBindTexture(GL_TEXTURE_2D, texture_id_);
        for (int i = 0; i < mip_levels; ++i) {
            const int mw = std::max(1, width >> i);
            const int mh = std::max(1, height >> i);
            glCompressedTexImage2D(GL_TEXTURE_2D, i, info.internal_format, mw, mh, 0,
                                   static_cast<GLsizei>(mip_sizes[i]), mip_data[i]);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        mip_levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip_levels - 1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    GLOG_INFO("Compressed texture created: {}x{} mips={} format={}",
              width, height, mip_levels, static_cast<int>(format));
    return true;
}

void GLTexture::bind(uint32_t slot) const {
    if (slot >= k_max_texture_slots) return;
    if (texture_id_ == 0) return;

    if (gl_dsa_available()) {
        if (g_bound_textures[slot] == texture_id_) return;
        glBindTextureUnit(slot, texture_id_);
        g_bound_textures[slot] = texture_id_;
    } else {
        if (g_active_texture_unit != static_cast<int>(slot)) {
            glActiveTexture(GL_TEXTURE0 + slot);
            g_active_texture_unit = static_cast<int>(slot);
        }
        if (g_bound_textures[slot] == texture_id_) return;
        glBindTexture(is_cubemap_ ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, texture_id_);
        g_bound_textures[slot] = texture_id_;
    }
}

void GLTexture::unbind() const {
    // 不维护缓存的精确清除，简单绑定 0 并清空当前 slot。
    if (g_active_texture_unit >= 0 && g_active_texture_unit < k_max_texture_slots) {
        g_bound_textures[g_active_texture_unit] = 0;
    }
    glBindTexture(is_cubemap_ ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, 0);
}

void GLTexture::set_filter(TextureFilter min, TextureFilter mag) {
    if (gl_dsa_available()) {
        glTextureParameteri(texture_id_, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(to_gl_filter(min)));
        glTextureParameteri(texture_id_, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(to_gl_filter(mag)));
    } else {
        const GLenum target = is_cubemap_ ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        glBindTexture(target, texture_id_);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(to_gl_filter(min)));
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(to_gl_filter(mag)));
        glBindTexture(target, 0);
    }
}

void GLTexture::set_wrap(TextureWrap s, TextureWrap t) {
    if (gl_dsa_available()) {
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_S, static_cast<GLint>(to_gl_wrap(s)));
        glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_T, static_cast<GLint>(to_gl_wrap(t)));
        if (is_cubemap_) {
            glTextureParameteri(texture_id_, GL_TEXTURE_WRAP_R, static_cast<GLint>(to_gl_wrap(s)));
        }
    } else {
        const GLenum target = is_cubemap_ ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        glBindTexture(target, texture_id_);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, static_cast<GLint>(to_gl_wrap(s)));
        glTexParameteri(target, GL_TEXTURE_WRAP_T, static_cast<GLint>(to_gl_wrap(t)));
        if (is_cubemap_) {
            glTexParameteri(target, GL_TEXTURE_WRAP_R, static_cast<GLint>(to_gl_wrap(s)));
        }
        glBindTexture(target, 0);
    }
}

uint32_t GLTexture::to_gl_filter(TextureFilter filter) {
    switch (filter) {
        case TextureFilter::Nearest:             return GL_NEAREST;
        case TextureFilter::Linear:                return GL_LINEAR;
        case TextureFilter::NearestMipmapNearest: return GL_NEAREST_MIPMAP_NEAREST;
        case TextureFilter::LinearMipmapLinear:   return GL_LINEAR_MIPMAP_LINEAR;
    }
    return GL_LINEAR;
}

uint32_t GLTexture::to_gl_wrap(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::Repeat:       return GL_REPEAT;
        case TextureWrap::ClampToEdge:  return GL_CLAMP_TO_EDGE;
        case TextureWrap::ClampToBorder: return GL_CLAMP_TO_BORDER;
    }
    return GL_REPEAT;
}

} // namespace gryce_engine::render
