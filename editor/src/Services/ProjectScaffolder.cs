using System;
using System.IO;
using System.Text;

namespace GryceEngine.Editor.Services;

/// <summary>
/// 新建项目脚手架：在指定目录生成标准的 Gryce 项目结构
/// （子目录 + project.gryce + project_settings.json + 默认场景 main.gesc）。
/// 参考 Godot 项目管理器的模板脚手架，空目录创建后即可用编辑器打开。
/// </summary>
public static class ProjectScaffolder
{
    /// <summary>项目根下创建的标准子目录。</summary>
    private static readonly string[] Subdirectories =
    {
        "audio", "fonts", "models", "scenes", "scripts", "shaders", "textures", "tilesets"
    };

    /// <summary>
    /// 创建项目。返回 null 表示成功，否则返回面向用户的错误提示。
    /// </summary>
    /// <param name="name">项目显示名（同时作为目录名）。</param>
    /// <param name="parentDir">项目要创建于其中的父目录（绝对路径）。</param>
    /// <param name="is3D">true=3D 空项目，false=2D 空项目。</param>
    public static string? Create(string name, string parentDir, bool is3D)
    {
        string projectName = name.Trim();
        if (string.IsNullOrWhiteSpace(projectName))
            return "new_project.name_required";
        if (string.IsNullOrWhiteSpace(parentDir) || !Directory.Exists(parentDir))
            return "new_project.bad_parent_dir";

        string root = Path.Combine(parentDir, projectName);
        if (Directory.Exists(root) && Directory.GetFileSystemEntries(root).Length > 0)
            return "new_project.already_exists";

        try
        {
            Directory.CreateDirectory(root);
            foreach (string sub in Subdirectories)
                Directory.CreateDirectory(Path.Combine(root, sub));

            WriteFile(Path.Combine(root, "project.gryce"), BuildProjectFile(projectName));
            WriteFile(Path.Combine(root, "project_settings.json"), BuildSettingsFile(is3D));

            string scenesDir = Path.Combine(root, "scenes");
            WriteFile(Path.Combine(scenesDir, "main.gesc"), BuildDefaultScene());
            return null;
        }
        catch (Exception ex)
        {
            return "new_project.create_failed:" + ex.Message;
        }
    }

    private static void WriteFile(string path, string content)
        => File.WriteAllText(path, content, new UTF8Encoding(false));

    private static string BuildProjectFile(string name)
    {
        return "{\n" +
               "  \"name\": \"" + JsonEscape(name) + "\",\n" +
               "  \"version\": \"0.1.0\",\n" +
               "  \"engine_version\": \">=0.1.0\",\n" +
               "  \"entry_scene\": \"res:/scenes/main.gesc\",\n" +
               "  \"physics\": {\n" +
               "    \"backend_3d\": \"jolt\"\n" +
               "  },\n" +
               "  \"window\": {\n" +
               "    \"width\": 1280,\n" +
               "    \"height\": 720,\n" +
               "    \"title\": \"" + JsonEscape(name) + "\"\n" +
               "  }\n" +
               "}\n";
    }

    private static string BuildSettingsFile(bool is3D)
    {
        // 参考 examples/3dtest 与 examples/2dDemo 的 project_settings.json。
        string mainScene = "\"main_scene\":\"res:/scenes/main.gesc\"";
        string scene2D = is3D ? "" : ",\"scene_2d\":true";
        return "{\"render_api\":\"opengl\",\"hdr\":true,\"tone_map_mode\":1," +
               "\"exposure\":1,\"shadow_enabled\":true,\"shadow_map_size\":2048," +
               "\"ambient_r\":0.15,\"ambient_g\":0.15,\"ambient_b\":0.15,\"ibl_intensity\":1" +
               scene2D + "," + mainScene + "}\n";
    }

    /// <summary>生成默认场景：MainCamera + MainLight（与编辑器"新建场景"骨架一致）。</summary>
    private static string BuildDefaultScene()
    {
        string camUuid = Guid.NewGuid().ToString();
        string lightUuid = Guid.NewGuid().ToString();
        return "{\n" +
               "  \"entities\": [\n" +
               "    {\n" +
               "      \"components\": [\n" +
               "        {\n" +
               "          \"background_color\": [0.12, 0.14, 0.18, 1.0],\n" +
               "          \"enabled\": true,\n" +
               "          \"far_plane\": 200.0,\n" +
               "          \"fov\": 60.0,\n" +
               "          \"is_main\": true,\n" +
               "          \"near_plane\": 0.1,\n" +
               "          \"type\": \"Camera\"\n" +
               "        }\n" +
               "      ],\n" +
               "      \"enabled\": true,\n" +
               "      \"name\": \"MainCamera\",\n" +
               "      \"parent\": null,\n" +
               "      \"transform\": { \"position\": [0.0, 5.0, 10.0], \"rotation\": [0.0, 0.0, 0.0, 1.0], \"scale\": [1.0, 1.0, 1.0] },\n" +
               "      \"uuid\": \"" + camUuid + "\"\n" +
               "    },\n" +
               "    {\n" +
               "      \"components\": [\n" +
               "        {\n" +
               "          \"color\": [1.0, 1.0, 1.0],\n" +
               "          \"direction\": [-0.5, -0.8, -0.3],\n" +
               "          \"enabled\": true,\n" +
               "          \"intensity\": 3.0,\n" +
               "          \"light_type\": 0,\n" +
               "          \"range\": 20.0,\n" +
               "          \"spot_angle\": 45.0,\n" +
               "          \"spot_softness\": 0.2,\n" +
               "          \"type\": \"Light\"\n" +
               "        }\n" +
               "      ],\n" +
               "      \"enabled\": true,\n" +
               "      \"name\": \"MainLight\",\n" +
               "      \"parent\": null,\n" +
               "      \"transform\": { \"position\": [0.0, 5.0, 2.0], \"rotation\": [0.0, 0.0, 0.0, 1.0], \"scale\": [1.0, 1.0, 1.0] },\n" +
               "      \"uuid\": \"" + lightUuid + "\"\n" +
               "    }\n" +
               "  ],\n" +
               "  \"name\": \"Scene\",\n" +
               "  \"version\": 2\n" +
               "}\n";
    }

    private static string JsonEscape(string value)
        => value.Replace("\\", "\\\\").Replace("\"", "\\\"");
}