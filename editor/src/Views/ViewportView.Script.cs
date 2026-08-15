using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using Microsoft.Web.WebView2.Core;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace GryceEngine.Editor.Views;

public partial class ViewportView
{

    // === Script-tab toolbar (undo / redo / save -> Monaco) ===

    private void OnScriptUndoClick(object sender, RoutedEventArgs e) => ScriptCommand("undo");

    private void OnScriptRedoClick(object sender, RoutedEventArgs e) => ScriptCommand("redo");

    private void OnScriptSaveClick(object sender, RoutedEventArgs e) => ScriptCommand("saveRequest");

    /// <summary>Forwards an editor command to the Monaco instance in the
    /// WebView2 script tab ("undo", "redo" or "saveRequest").</summary>


    /// <summary>Forwards an editor command to the Monaco instance in the
    /// WebView2 script tab ("undo", "redo" or "saveRequest").</summary>
    private void ScriptCommand(string cmd)
    {
        try
        {
            if (ScriptWebView.CoreWebView2 == null) return;
            ScriptWebView.Focus();
            if (cmd == "saveRequest")
            {
                ScriptWebView.CoreWebView2.ExecuteScriptAsync(
                    "window.chrome.webview.postMessage(" +
                    "{ cmd: 'save', text: window.gryceEditor.getContent() });");
            }
            else
            {
                ScriptWebView.CoreWebView2.ExecuteScriptAsync(
                    "window.gryceEditor && window.gryceEditor." + cmd + "();");
            }
        }
        catch { /* webview not ready */ }
    }



    private void EnterScriptMode(string? path)
    {
        _scriptMode = true;
        // Hidden (not Collapsed): Collapsed destroys the HwndHost window and
        // the embedded GLFW child together with its GL context, leaving the
        // sleeping render thread holding a dead context. Hidden keeps the
        // window alive (just not shown), so switching back resumes rendering
        // without a destroy/recreate race.
        HostContainer.Visibility = Visibility.Hidden;
        GizmoOverlay.Visibility = Visibility.Collapsed;
        ViewportOverlay.Visibility = Visibility.Collapsed;
        ScriptWebView.Visibility = Visibility.Visible;
        // 代码编辑器里不显示场景编辑器工具：顶栏右侧的显示模式/分辨率、
        // 底部信息栏，以及悬浮的 Gizmo 覆盖窗口。
        UpdateSceneToolsVisible(false);
        UpdateScriptToolsVisible(true);
        _overlay?.Hide();
        ScriptModeChanged?.Invoke(false);
        LoadScriptFile(path);
        // Keep keyboard focus inside Monaco so Ctrl+Z / Ctrl+Y / Ctrl+S edit
        // the script text immediately after switching to the Script tab.
        Dispatcher.BeginInvoke(new Action(() =>
        {
            try
            {
                ScriptWebView.Focus();
                ScriptWebView.CoreWebView2?.ExecuteScriptAsync(
                    "window.gryceEditor && window.gryceEditor.focus();");
            }
            catch { /* webview not ready yet */ }
        }), DispatcherPriority.Input);
    }



    private void OnOpenScriptRequested(string path)
    {
        Dispatcher.BeginInvoke(new Action(() =>
        {
            TabScene.IsChecked = false;
            Tab2D.IsChecked = false;
            TabGame.IsChecked = false;
            TabScript.IsChecked = true;
            EnterScriptMode(path);
        }), DispatcherPriority.Normal);
    }

    // === Script tab: Monaco (WebView2) ===



    // === Script tab: Monaco (WebView2) ===

    private async void LoadScriptFile(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
        {
            path = FindNewScriptPath();
        }
        _currentScriptPath = path;
        try
        {
            _scriptInitialContent = File.Exists(path) ? File.ReadAllText(path) : NewScriptTemplate;
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Script] read failed: {ex.Message}");
            _scriptInitialContent = string.Empty;
        }

        try
        {
            await EnsureScriptWebViewAsync();
            if (!_scriptWebReady)
            {
                _scriptLoadingContent = true;
                var html = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory,
                                                  "assets", "monaco", "editor.html");
                ScriptWebView.CoreWebView2.Navigate(new Uri(html).ToString());
            }
            else
            {
                SendContentToWeb(_scriptInitialContent);
            }
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Script] WebView error: {ex.Message}");
        }
    }



    private async System.Threading.Tasks.Task EnsureScriptWebViewAsync()
    {
        if (ScriptWebView.CoreWebView2 != null) return;
        await ScriptWebView.EnsureCoreWebView2Async(null);
        if (ScriptWebView.CoreWebView2 == null) return;
        ScriptWebView.CoreWebView2.WebMessageReceived += OnScriptWebMessage;
        ScriptWebView.CoreWebView2.NavigationCompleted += OnScriptNavigationCompleted;
        ScriptWebView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = true;
        ScriptWebView.CoreWebView2.Settings.AreDevToolsEnabled = false;
    }



    private void OnScriptNavigationCompleted(object? sender, CoreWebView2NavigationCompletedEventArgs e)
    {
        if (!e.IsSuccess || _scriptLoadingContent)
        {
            _scriptLoadingContent = false;
            SendContentToWeb(_scriptInitialContent);
        }
    }



    private void SendContentToWeb(string? content)
    {
        if (ScriptWebView.CoreWebView2 == null) return;
        var payload = "{\"cmd\":\"setContent\",\"content\":\"" +
                      JsonEscape(content ?? string.Empty) + "\"}";
        try
        {
            ScriptWebView.CoreWebView2.PostWebMessageAsJson(payload);
            // Keep keyboard focus inside Monaco so Ctrl+Z / Ctrl+Y edit the
            // script text instead of falling through to the scene undo/redo.
            ScriptWebView.Focus();
            ScriptWebView.CoreWebView2.ExecuteScriptAsync(
                "window.gryceEditor && window.gryceEditor.focus();");
        }
        catch { /* page not ready yet */ }
    }



    private static string JsonEscape(string s)
    {
        var sb = new StringBuilder(s.Length + 16);
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        return sb.ToString();
    }



    private void OnScriptWebMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        string json;
        try { json = e.TryGetWebMessageAsString(); }
        catch { return; }

        string cmd = ExtractJsonString(json, "cmd") ?? string.Empty;
        if (cmd == "ready")
        {
            _scriptWebReady = true;
            SendContentToWeb(_scriptInitialContent);
        }
        else if (cmd == "save")
        {
            string text = ExtractJsonString(json, "text") ?? string.Empty;
            SaveScript(text);
        }
        else if (cmd == "changed")
        {
            if (VM != null && !string.IsNullOrEmpty(_currentScriptPath))
            {
                VM.AppendConsole($"[Script] modified: {System.IO.Path.GetFileName(_currentScriptPath)}");
            }
        }
    }



    private static string? ExtractJsonString(string json, string key)
    {
        var marker = "\"" + key + "\":";
        int idx = json.IndexOf(marker, StringComparison.Ordinal);
        if (idx < 0) return null;
        idx += marker.Length;
        while (idx < json.Length && char.IsWhiteSpace(json[idx])) idx++;
        if (idx >= json.Length || json[idx] != '"') return null;
        var sb = new StringBuilder();
        bool escape = false;
        for (int i = idx + 1; i < json.Length; i++)
        {
            char c = json[i];
            if (escape)
            {
                sb.Append(c);
                escape = false;
            }
            else if (c == '\\') escape = true;
            else if (c == '"') break;
            else sb.Append(c);
        }
        return sb.ToString();
    }



    private void SaveScript(string text)
    {
        if (string.IsNullOrEmpty(_currentScriptPath)) return;
        try
        {
            File.WriteAllText(_currentScriptPath, text);
            VM?.AppendConsole($"Saved: {_currentScriptPath}");
            VM?.ReloadScripts();
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Script] save failed: {ex.Message}");
        }
    }



    private static string FindNewScriptPath()
    {
        string root = EngineService.Current?.ProjectRoot ?? AppDomain.CurrentDomain.BaseDirectory;
        string scripts = System.IO.Path.Combine(root, "scripts");
        try { Directory.CreateDirectory(scripts); } catch { }
        for (int i = 1; ; i++)
        {
            string candidate = System.IO.Path.Combine(scripts, $"NewScript{i}.lua");
            if (!File.Exists(candidate)) return candidate;
        }
    }



    private const string NewScriptTemplate =
        "-- GryceSRT script template (GryceEngineUtils preloaded).\r\n" +
        "require(\"GryceEngineUtils\")\r\n" +
        "\r\n" +
        "props = {\r\n" +
        "    speed = 1.0\r\n" +
        "}\r\n" +
        "\r\n" +
        "function on_start()\r\n" +
        "    engine.log.info(\"on_start\")\r\n" +
        "end\r\n" +
        "\r\n" +
        "function on_update(dt)\r\n" +
        "    -- engine.self() / engine.entity.* / engine.component.* / GryceEngineUtils.*\r\n" +
        "end\r\n";


}
