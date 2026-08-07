using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Windows;

namespace GryceEngine.Editor.Services;

/// <summary>Editor languages supported by the localization service.</summary>
public enum EditorLanguage
{
    English = 0,
    Chinese
}

/// <summary>
/// Runtime localization service.
/// Locale tables are embedded resources (project/locales/{en,zh}.json).
/// Missing keys fall back to English, then to the key itself so unfinished
/// translations never break the UI.
/// </summary>
public sealed class LocalizationService : INotifyPropertyChanged
{
    private static readonly string[] ResourceNames =
    {
        "GryceEngine.Editor.project.locales.en.json",
        "GryceEngine.Editor.project.locales.zh.json"
    };

    public static LocalizationService Instance { get; } = new();

    private readonly Dictionary<string, string> _english = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _current = new(StringComparer.Ordinal);
    private EditorLanguage _language = EditorLanguage.English;

    public EditorLanguage Language
    {
        get => _language;
        set
        {
            if (_language == value) return;
            _language = value;
            LoadCurrentTable();
            OnPropertyChanged("Item[]");
            LanguageChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public string LanguageCode => _language switch
    {
        EditorLanguage.English => "en",
        EditorLanguage.Chinese => "zh",
        _ => "en"
    };

    public string LanguageDisplayName => _language switch
    {
        EditorLanguage.English => "English",
        EditorLanguage.Chinese => "中文 (Chinese)",
        _ => "English"
    };

    /// <summary>Indexer used by XAML bindings: {Binding [menu.file], Source=...}.</summary>
    public string this[string key] => T(key);

    public event EventHandler? LanguageChanged;

    private LocalizationService()
    {
        LoadResource(ResourceNames[0], _english);
        LoadCurrentTable();
    }

    /// <summary>Translate a key. Falls back to English, then to the key itself.</summary>
    public string T(string key)
    {
        if (string.IsNullOrEmpty(key)) return key ?? string.Empty;
        if (_current.TryGetValue(key, out var value)) return value;
        if (_english.TryGetValue(key, out value)) return value;
        return key;
    }

    private void LoadCurrentTable()
    {
        _current.Clear();
        // English is always the base; merge the selected language on top.
        foreach (var kv in _english) _current[kv.Key] = kv.Value;
        if (_language == EditorLanguage.English) return;
        var dict = new Dictionary<string, string>();
        LoadResource(ResourceNames[(int)_language], dict);
        foreach (var kv in dict) _current[kv.Key] = kv.Value;
    }

    private static void LoadResource(string resourceName, Dictionary<string, string> target)
    {
        try
        {
            var assembly = typeof(LocalizationService).Assembly;
            using var stream = assembly.GetManifestResourceStream(resourceName);
            if (stream == null) return;
            using var reader = new StreamReader(stream);
            string json = reader.ReadToEnd();
            var table = ParseJsonTable(json);
            foreach (var kv in table) target[kv.Key] = kv.Value;
        }
        catch (Exception ex)
        {
            // Localization must never crash the editor; log and continue with fallbacks.
            System.Diagnostics.Debug.WriteLine($"[Localization] failed to load {resourceName}: {ex.Message}");
        }
    }

    /// <summary>
    /// Minimal JSON object parser for flat string:string tables.
    /// Avoids a JSON dependency in the editor project.
    /// </summary>
    private static Dictionary<string, string> ParseJsonTable(string json)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        int i = 0;
        SkipWhitespace(json, ref i);
        if (i >= json.Length || json[i] != '{') return result;
        i++; // consume '{'

        while (i < json.Length)
        {
            SkipWhitespace(json, ref i);
            if (i >= json.Length) break;
            if (json[i] == '}') break;
            if (json[i] == ',') { i++; continue; }

            string key = ReadString(json, ref i);
            SkipWhitespace(json, ref i);
            if (i < json.Length && json[i] == ':') i++;
            SkipWhitespace(json, ref i);
            string value = ReadString(json, ref i);
            result[key] = value;
        }
        return result;
    }

    private static string ReadString(string json, ref int i)
    {
        SkipWhitespace(json, ref i);
        if (i >= json.Length || json[i] != '"') return string.Empty;
        i++; // opening quote
        var sb = new System.Text.StringBuilder();
        while (i < json.Length)
        {
            char c = json[i];
            if (c == '\\' && i + 1 < json.Length)
            {
                char next = json[i + 1];
                switch (next)
                {
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    case '/': sb.Append('/'); break;
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'u':
                        if (i + 5 < json.Length &&
                            int.TryParse(json.Substring(i + 2, 4), System.Globalization.NumberStyles.HexNumber,
                                         System.Globalization.CultureInfo.InvariantCulture, out int code))
                        {
                            sb.Append((char)code);
                            i += 4;
                        }
                        break;
                    default: sb.Append(next); break;
                }
                i += 2;
                continue;
            }
            if (c == '"') { i++; break; }
            sb.Append(c);
            i++;
        }
        return sb.ToString();
    }

    private static void SkipWhitespace(string json, ref int i)
    {
        while (i < json.Length && char.IsWhiteSpace(json[i])) i++;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
