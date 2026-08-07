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
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[EditorSettings] load failed: {ex.Message}");
        }
        return result;
    }

    public static void Save(string language, string theme)
    {
        try
        {
            string json = "{\"language\":\"" + Escape(language) +
                          "\",\"theme\":\"" + Escape(theme) + "\"}";
            File.WriteAllText(SettingsPath, json);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[EditorSettings] save failed: {ex.Message}");
        }
    }

    private static string? ReadString(string json, string key)
    {
        var match = Regex.Match(json, "\"" + Regex.Escape(key) + "\"\\s*:\\s*\"([^\"]*)\"");
        return match.Success ? match.Groups[1].Value : null;
    }

    private static string Escape(string value) =>
        value.Replace("\\", "\\\\").Replace("\"", "\\\"");
}
