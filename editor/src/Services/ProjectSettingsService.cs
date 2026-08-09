using System;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Project-level settings（渲染后端、管线质量），持久化到项目根目录的
/// project_settings.json。后端与阴影贴图尺寸在渲染器初始化（重启）时生效，
/// 其余质量项可运行时调整。
/// </summary>
public static class ProjectSettingsService
{
    public sealed class ProjectSettings
    {
        public string RenderApi { get; set; } = "vulkan"; // "vulkan" | "opengl"（重启生效）
        public bool Hdr { get; set; } = true;
        public int ToneMapMode { get; set; } = 1;   // 0=None, 1=Reinhard, 2=ACES
        public float Exposure { get; set; } = 1.0f;
        public bool ShadowEnabled { get; set; } = true;
        public int ShadowMapSize { get; set; } = 2048; // 重启生效
        public float AmbientR { get; set; } = 0.15f;
        public float AmbientG { get; set; } = 0.15f;
        public float AmbientB { get; set; } = 0.15f;
        public float IblIntensity { get; set; } = 1.0f;
    }

    public static string ProjectPath
    {
        get
        {
            string? root = App.Engine?.ProjectRoot;
            return string.IsNullOrWhiteSpace(root)
                ? Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "project_settings.json")
                : Path.Combine(root, "project_settings.json");
        }
    }

    public static ProjectSettings Load()
    {
        var s = new ProjectSettings();
        try
        {
            if (!File.Exists(ProjectPath)) return s;
            string json = File.ReadAllText(ProjectPath);
            s.RenderApi = ReadString(json, "render_api") ?? "vulkan";
            s.Hdr = ReadBool(json, "hdr") ?? true;
            s.ToneMapMode = ReadInt(json, "tone_map_mode") ?? 1;
            s.Exposure = ReadFloat(json, "exposure") ?? 1.0f;
            s.ShadowEnabled = ReadBool(json, "shadow_enabled") ?? true;
            s.ShadowMapSize = ReadInt(json, "shadow_map_size") ?? 2048;
            s.AmbientR = ReadFloat(json, "ambient_r") ?? 0.15f;
            s.AmbientG = ReadFloat(json, "ambient_g") ?? 0.15f;
            s.AmbientB = ReadFloat(json, "ambient_b") ?? 0.15f;
            s.IblIntensity = ReadFloat(json, "ibl_intensity") ?? 1.0f;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[ProjectSettings] load failed: {ex.Message}");
        }
        return s;
    }

    public static void Save(ProjectSettings s)
    {
        try
        {
            string json =
                "{\"render_api\":\"" + Escape(s.RenderApi) + "\"" +
                ",\"hdr\":" + (s.Hdr ? "true" : "false") +
                ",\"tone_map_mode\":" + s.ToneMapMode +
                ",\"exposure\":" + F(s.Exposure) +
                ",\"shadow_enabled\":" + (s.ShadowEnabled ? "true" : "false") +
                ",\"shadow_map_size\":" + s.ShadowMapSize +
                ",\"ambient_r\":" + F(s.AmbientR) +
                ",\"ambient_g\":" + F(s.AmbientG) +
                ",\"ambient_b\":" + F(s.AmbientB) +
                ",\"ibl_intensity\":" + F(s.IblIntensity) + "}";
            File.WriteAllText(ProjectPath, json);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[ProjectSettings] save failed: {ex.Message}");
        }
    }

    /// <summary>把设置应用到渲染器；后端与阴影贴图尺寸在下次初始化（重启）生效。</summary>
    public static void Apply(ProjectSettings s)
    {
        try
        {
            Native.RenderAPI.GRender_SetHDR(s.Hdr);
            Native.RenderAPI.GRender_SetToneMapMode(s.ToneMapMode);
            Native.RenderAPI.GRender_SetExposure(s.Exposure);
            Native.RenderAPI.GRender_SetShadowEnabled(s.ShadowEnabled);
            Native.RenderAPI.GRender_SetShadowMapSize(s.ShadowMapSize);
            Native.RenderAPI.GRender_SetAmbient(s.AmbientR, s.AmbientG, s.AmbientB);
            Native.RenderAPI.GRender_SetIBLIntensity(s.IblIntensity);
            Native.RenderAPI.GRender_RequestBackend(
                string.Equals(s.RenderApi, "opengl", System.StringComparison.OrdinalIgnoreCase)
                    ? Native.GRenderAPI.OpenGL
                    : Native.GRenderAPI.Vulkan);
        }
        catch { /* renderer 可能尚未初始化；值已存入 Core，init 时应用 */ }
    }

    private static string F(float v) => v.ToString("0.###", CultureInfo.InvariantCulture);

    private static string? ReadString(string json, string key)
    {
        var m = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*\"([^\"]*)\"");
        return m.Success ? m.Groups[1].Value : null;
    }

    private static bool? ReadBool(string json, string key)
    {
        var m = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*(true|false)");
        return m.Success ? string.Equals(m.Groups[1].Value, "true", StringComparison.OrdinalIgnoreCase) : null;
    }

    private static int? ReadInt(string json, string key)
    {
        var m = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*(-?\\d+)");
        return m.Success && int.TryParse(m.Groups[1].Value, out int v) ? v : null;
    }

    private static float? ReadFloat(string json, string key)
    {
        var m = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
        return m.Success && float.TryParse(m.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float v)
            ? v
            : null;
    }

    private static string Escape(string value) =>
        value.Replace("\\", "\\\\").Replace("\"", "\\\"");
}
