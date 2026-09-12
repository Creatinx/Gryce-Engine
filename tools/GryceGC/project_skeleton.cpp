#include "project_skeleton.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "string_util.h"

namespace fs = std::filesystem;

namespace gryce_engine::gc {

// 创建标准 GryceGC-A 项目骨架。结构见 docs/GryceGC-A.md §2：
//   <dir>/project.data            唯一配置文件（项目清单 + 运行时设置合并于一处，JSON）
//   <dir>/scenes/                 场景 .gesc
//   <dir>/scripts/                 JS 脚本 .js（ES Module，QuickJS 运行时）
//   <dir>/shaders/                 着色器
//   <dir>/models/                  模型
//   <dir>/textures/                贴图
//   <dir>/audio/                   音频
//   <dir>/fonts/                   字体
//   <dir>/tilesets/                Tilemap 瓦片集
// 其中自动生成一个最小的空主场景 scenes/main.gesc（v2 格式，含一个空根）。
bool create_project_skeleton(const fs::path& dir, const std::string& name,
                             const std::string& window_title) {
    std::error_code ec;
    // 目录必须不存在或为空，避免覆盖已有项目。
    if (fs::exists(dir, ec)) {
        if (fs::is_directory(dir, ec) && !fs::is_empty(dir, ec)) {
            std::cerr << "[grycegc] ERROR: " << dir
                      << " 已存在且非空，拒绝覆盖现有项目\n";
            return false;
        }
        fs::remove_all(dir, ec);
        if (ec) {
            std::cerr << "[grycegc] ERROR: failed to clear " << dir
                      << ": " << ec.message() << "\n";
            return false;
        }
    }

    const std::vector<std::string> subdirs = {
        "scenes", "scripts", "shaders", "models",
        "textures", "audio", "fonts", "tilesets",
    };
    for (const std::string& sub : subdirs) {
        if (!fs::create_directories(dir / sub, ec) && ec) {
            std::cerr << "[grycegc] ERROR: failed to create " << (dir / sub)
                      << ": " << ec.message() << "\n";
            return false;
        }
    }

    // project.data —— 唯一配置文件：项目清单 + 运行时设置合并于一处
    //（原 project_settings.json 的内容并入此文件顶层，作为运行时设置；打包时还会
    // 附加原 gdata 的打包元数据字段。）
    {
        std::string title = window_title.empty() ? name : window_title;
        std::string j = "{\n"
            "  \"name\": \"" + json_escape(name) + "\",\n"
            "  \"version\": \"0.1.0\",\n"
            "  \"engine_version\": \">=0.1.0\",\n"
            "  \"entry_scene\": \"res:/scenes/main.gesc\",\n"
            "  \"physics\": { \"backend_2d\": \"box2d\", \"backend_3d\": \"jolt\" },\n"
            "  \"window\": { \"width\": 1280, \"height\": 720, \"title\": \""
            + json_escape(title) + "\" },\n"
            "  \"render_api\": \"opengl\",\n"
            "  \"hdr\": true,\n"
            "  \"tone_map_mode\": 1,\n"
            "  \"exposure\": 1.0,\n"
            "  \"shadow_enabled\": true,\n"
            "  \"shadow_map_size\": 2048,\n"
            "  \"ambient_r\": 0.2,\n"
            "  \"ambient_g\": 0.22,\n"
            "  \"ambient_b\": 0.26,\n"
            "  \"ibl_intensity\": 1.0,\n"
            "  \"main_scene\": \"res:/scenes/main.gesc\"\n"
            "}\n";
        std::ofstream out(dir / "project.data");
        if (!out) {
            std::cerr << "[grycegc] ERROR: failed to write project.data\n";
            return false;
        }
        out << j;
        if (!out.good()) return false;
    }

    // scenes/main.gesc —— 最小空主场景（v2：单合成根，无落盘实体）
    {
        const char* scene =
            "{\n"
            "  \"version\": 2,\n"
            "  \"name\": \"Main\",\n"
            "  \"entities\": []\n"
            "}\n";
        std::ofstream out(dir / "scenes" / "main.gesc");
        if (!out) {
            std::cerr << "[grycegc] ERROR: failed to write scenes/main.gesc\n";
            return false;
        }
        out << scene;
        if (!out.good()) return false;
    }

    // 各资源目录占位说明（可选，保持空目录在 VCS 中可见）
    for (const std::string& sub : subdirs) {
        if (sub == "scenes") continue;
        const fs::path keep = dir / sub / ".gitkeep";
        std::ofstream out(keep);
        out << "# " << sub << " 资源目录（GryceGC-A）\n";
    }

    std::printf("[grycegc] created GryceGC-A project '%s' at %s\n",
                name.c_str(), dir.string().c_str());
    std::printf("[grycegc]   scenes/main.gesc  -> 主场景（入口）\n");
    std::printf("[grycegc]   project.data     -> 唯一配置文件（清单 + 运行时设置）\n");
    std::printf("[grycegc] 下一步: 将场景/脚本/资源放入对应分类目录，然后\n");
    std::printf("[grycegc]   GryceGC --project %s --name %s --build-dir build --config Release --out build/game\n",
                dir.string().c_str(), name.c_str());
    return true;
}

} // namespace gryce_engine::gc