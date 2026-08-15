using System;
using System.IO;
using System.Text.RegularExpressions;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Minimal editor settings persistence (language + theme).
/// Stored as a tiny JSON file next to the executable.
/// </summary>
public static class EditorSettingsService
{
    public sealed class Settings
    {
        public string Language { get; set; } = "en";
        public string Theme { get; set; } = "Dark";
        public bool MicaBackdrop { get; set; } = true;
        public int AutoSaveInterval { get; set; } = 300; // 分钟；0 = 关闭
        public bool VSync { get; set; } = true;
        public bool ShowGrid { get; set; } = true;
        public bool ShowGizmos { get; set; } = true;
        public bool ShowStats { get; set; } = false;
        // 上次打开的项目根（首启直接恢复，不再总是落到 examples/3dtest）
        public string LastProject { get; set; } = "";
    }

    private static string SettingsPath =>
        Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "editor_settings.json");

    public static Settings Load()
    {
        var result = new Settings();
        try
        {
            if (!File.Exists(SettingsPath)) return result;
            string json = File.ReadAllText(SettingsPath);
            result.Language = ReadString(json, "language") ?? "en";
            result.Theme = ReadString(json, "theme") ?? "Dark";
            result.MicaBackdrop = ReadBool(json, "mica") ?? true;
            result.AutoSaveInterval = ReadInt(json, "autosave_interval") ?? 300;
            result.VSync = ReadBool(json, "vsync") ?? true;
            result.ShowGrid = ReadBool(json, "show_grid") ?? true;
            result.ShowGizmos = ReadBool(json, "show_gizmos") ?? true;
            result.ShowStats = ReadBool(json, "show_stats") ?? false;
            result.LastProject = ReadString(json, "last_project") ?? "";
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[EditorSettings] load failed: {ex.Message}");
        }
        return result;
    }

    public static void Save(string language, string theme, bool mica, int autoSaveInterval,
                            bool vsync, bool showGrid, bool showGizmos, bool showStats,
                            string lastProject = "")
    {
        try
        {
            string json = "{\"language\":\"" + Escape(language) +
                          "\",\"theme\":\"" + Escape(theme) +
                          "\",\"mica\":" + (mica ? "true" : "false") +
                          ",\"autosave_interval\":" + autoSaveInterval +
                          ",\"vsync\":" + (vsync ? "true" : "false") +
                          ",\"show_grid\":" + (showGrid ? "true" : "false") +
                          ",\"show_gizmos\":" + (showGizmos ? "true" : "false") +
                          ",\"show_stats\":" + (showStats ? "true" : "false") +
                          ",\"last_project\":\"" + Escape(lastProject) + "\"}";
            File.WriteAllText(SettingsPath, json);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[EditorSettings] save failed: {ex.Message}");
        }
    }

    /// <summary>持久化"上次打开的项目"，保留其余设置不变。</summary>
    public static void SaveLastProject(string projectRoot)
    {
        var s = Load();
        Save(s.Language, s.Theme, s.MicaBackdrop, s.AutoSaveInterval, s.VSync,
             s.ShowGrid, s.ShowGizmos, s.ShowStats, projectRoot);
    }

    // -----------------------------------------------------------------------
    // 最近项目列表（recent_projects.json，独立于 editor_settings.json，
    // 避免改动手动拼接的设置 JSON 结构）
    // -----------------------------------------------------------------------
    private static string RecentProjectsPath =>
        Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "recent_projects.json");

    private const int MaxRecentProjects = 6;

    public static List<string> GetRecentProjects()
    {
        var result = new List<string>();
        try
        {
            if (!File.Exists(RecentProjectsPath)) return result;
            string json = File.ReadAllText(RecentProjectsPath);
            // 极简解析：["a","b",...] 或 {"projects":[...]}
            var match = Regex.Match(json, "\"[^\"]*\"");
            while (match.Success)
            {
                string value = match.Value.Trim('"');
                if (Directory.Exists(value) && !result.Contains(value, StringComparer.OrdinalIgnoreCase))
                    result.Add(value);
                match = match.NextMatch();
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[RecentProjects] load failed: {ex.Message}");
        }
        return result;
    }

    /// <summary>把项目加入最近列表（去重、置顶、截断），并持久化。</summary>
    public static void AddRecentProject(string projectRoot)
    {
        if (string.IsNullOrWhiteSpace(projectRoot)) return;
        try
        {
            var list = GetRecentProjects();
            list.RemoveAll(p => string.Equals(p, projectRoot, StringComparison.OrdinalIgnoreCase));
            list.Insert(0, projectRoot);
            if (list.Count > MaxRecentProjects)
                list.RemoveRange(MaxRecentProjects, list.Count - MaxRecentProjects);
            File.WriteAllText(RecentProjectsPath,
                "{\"projects\":[\"" + string.Join("\",\"", list) + "\"]}",
                new System.Text.UTF8Encoding(false));
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[RecentProjects] save failed: {ex.Message}");
        }
    }

    private static string? ReadString(string json, string key)
    {
        var match = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*\"([^\"]*)\"");
        return match.Success ? match.Groups[1].Value : null;
    }

    private static bool? ReadBool(string json, string key)
    {
        var match = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*(true|false)");
        if (!match.Success) return null;
        return string.Equals(match.Groups[1].Value, "true", StringComparison.OrdinalIgnoreCase);
    }

    private static int? ReadInt(string json, string key)
    {
        var match = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*(\\d+)");
        if (!match.Success) return null;
        return int.TryParse(match.Groups[1].Value, out int value) ? value : null;
    }

    private static string Escape(string value) =>
        value.Replace("\\", "\\\\").Replace("\"", "\\\"");
}
