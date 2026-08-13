using GryceEngine.Editor.Services;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Windows;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Packaging setup dialog: pick platform, game name and output directory,
/// then run GryceGC (grycegc.exe). On success the dialog closes and the caller
/// shows the result popup.
/// </summary>
public partial class PackageDialog : Window
{
    private readonly string _engineRoot;
    private readonly string _projectRoot;
    private bool _running;
    private string? _toolConfig;
    private string? _toolBuildDir;

    /// <summary>Console/log lines produced while packaging.</summary>
    public List<string> OutputLines { get; } = new();

    /// <summary>True when grycegc completed with exit code 0.</summary>
    public bool Succeeded { get; private set; }

    /// <summary>The packaged game output directory (out dir + game name).</summary>
    public string? LastOutputDir { get; private set; }

    /// <summary>Raised for each packaging output line (already on UI thread).</summary>
    public event Action<string>? Log;

    public PackageDialog(string engineRoot, string projectRoot)
    {
        InitializeComponent();
        _engineRoot = engineRoot;
        _projectRoot = projectRoot;
        OutputDirBox.Text = Path.Combine(engineRoot, "build", "game");
    }

    /// <summary>Locates grycegc.exe in the engine build output. Prefers the
    /// config matching the editor build (Debug when the editor is a Debug
    /// build), falls back to the other config.
    /// Tries both multi-config (VS: build/bin/config) and single-config
    /// (Ninja: build/config/bin/config) layouts.</summary>
    private string? FindGryceGCTool()
    {
        bool editorDebug = AppDomain.CurrentDomain.BaseDirectory.Contains("Debug");
        string[] candidates = editorDebug
            ? new[] { "Debug", "Release" }
            : new[] { "Release", "Debug" };
        foreach (string config in candidates)
        {
            // Multi-config (Visual Studio generator): output to build/bin/config
            string exe = Path.Combine(_engineRoot, "build", "bin", config, "grycegc.exe");
            if (File.Exists(exe))
            {
                _toolConfig = config;
                _toolBuildDir = Path.Combine(_engineRoot, "build");
                return exe;
            }
            // Single-config (Ninja generator): output to build/config/bin/config
            string exeNinja = Path.Combine(_engineRoot, "build", config, "bin", config, "grycegc.exe");
            if (File.Exists(exeNinja))
            {
                _toolConfig = config;
                _toolBuildDir = Path.Combine(_engineRoot, "build", config);
                return exeNinja;
            }
        }
        return null;
    }

    private async System.Threading.Tasks.Task<bool> BuildGryceGCToolAsync(string config)
    {
        StatusText.Text = $"Building GryceGC ({config})...";
        // Single-config (Ninja) trees put the tool under build/<config>/bin/<config>;
        // multi-config (Visual Studio) trees under build/bin/<config>. Detect the
        // layout and build the correct tree so the freshly built tool is found.
        string buildDir = Path.Combine(_engineRoot, "build");
        if (File.Exists(Path.Combine(buildDir, "CMakeCache.txt")) &&
            System.IO.File.ReadAllText(Path.Combine(buildDir, "CMakeCache.txt")) is string cache &&
            cache.Contains("CMAKE_GENERATOR:INTERNAL=Ninja"))
        {
            buildDir = Path.Combine(buildDir, config);
        }
        var psi = new ProcessStartInfo("cmake")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            Arguments =
                $"--build \"{buildDir}\" " +
                $"--target GryceGC --config {config}"
        };
        try
        {
            using var proc = Process.Start(psi);
            if (proc == null) return false;
            string output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            foreach (string raw in output.Split('\n'))
            {
                string line = raw.TrimEnd('\r');
                if (!string.IsNullOrWhiteSpace(line)) LogLine("cmake: " + line.Trim());
            }
            return proc.ExitCode == 0 && FindGryceGCTool() != null;
        }
        catch (Exception ex)
        {
            LogLine("cmake failed: " + ex.Message);
            return false;
        }
    }

    private void OnBrowseClick(object sender, RoutedEventArgs e)
    {
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = LocalizationService.Instance.T("package.output_dir"),
            ShowNewFolderButton = true,
            SelectedPath = Directory.Exists(OutputDirBox.Text) ? OutputDirBox.Text : _engineRoot
        };
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            OutputDirBox.Text = dialog.SelectedPath;
        }
    }

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }

    private async void OnPackClick(object sender, RoutedEventArgs e)
    {
        if (_running) return;

        string name = NameBox.Text.Trim();
        string outDir = OutputDirBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(outDir))
        {
            StatusText.Text = LocalizationService.Instance.T("package.bad_output_dir");
            return;
        }

        string? tool = FindGryceGCTool();
        if (tool == null)
        {
            // Not built yet: compile the GryceGC target first (Debug or
            // Release depending on the editor build), then continue.
            _running = true;
            SetBusy(true);
            bool built = await BuildGryceGCToolAsync(
                AppDomain.CurrentDomain.BaseDirectory.Contains("Debug") ? "Debug" : "Release");
            _running = false;
            SetBusy(false);
            tool = FindGryceGCTool();
            if (!built || tool == null)
            {
                StatusText.Text = LocalizationService.Instance.T("package.tool_missing");
                return;
            }
        }

        _running = true;
        SetBusy(true);
        StatusText.Text = LocalizationService.Instance.T("package.packing") +
                          $" ({_toolConfig})";

        string args =
            $"--project \"{_projectRoot}\" --name \"{name}\" " +
            $"--build-dir \"{_toolBuildDir}\" --config {_toolConfig} " +
            $"--out \"{outDir}\"";
        LogLine($"Package: {tool} {args}");

        var result = await System.Threading.Tasks.Task.Run(() => RunPackager(tool, args));

        foreach (string raw in result.Output.Split('\n'))
        {
            string line = raw.TrimEnd('\r');
            if (!string.IsNullOrWhiteSpace(line)) LogLine(line.Trim());
        }

        if (result.ExitCode == 0)
        {
            Succeeded = true;
            LastOutputDir = Path.Combine(outDir, name);
            DialogResult = true;
        }
        else
        {
            _running = false;
            SetBusy(false);
            StatusText.Text = string.Format(
                LocalizationService.Instance.T("package.failed"), result.ExitCode);
        }
    }

    private void LogLine(string line)
    {
        OutputLines.Add(line);
        Log?.Invoke(line);
    }

    private void SetBusy(bool busy)
    {
        PackButton.IsEnabled = !busy;
        CancelButton.IsEnabled = !busy;
        NameBox.IsEnabled = !busy;
        OutputDirBox.IsEnabled = !busy;
        PlatformBox.IsEnabled = !busy;
    }

    private static (int ExitCode, string Output) RunPackager(string tool, string args)
    {
        try
        {
            var psi = new ProcessStartInfo(tool)
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                Arguments = args
            };
            using var proc = Process.Start(psi);
            if (proc == null) return (-1, "failed to start grycegc");
            string output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            return (proc.ExitCode, output);
        }
        catch (Exception ex)
        {
            return (-1, ex.Message);
        }
    }
}
