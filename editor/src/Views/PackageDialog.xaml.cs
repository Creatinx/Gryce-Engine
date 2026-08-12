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

        string tool = Path.Combine(_engineRoot, "build", "bin", "Release", "grycegc.exe");
        if (!File.Exists(tool))
        {
            StatusText.Text = LocalizationService.Instance.T("package.tool_missing");
            return;
        }

        _running = true;
        SetBusy(true);
        StatusText.Text = LocalizationService.Instance.T("package.packing");

        string args =
            $"--project \"{_projectRoot}\" --name \"{name}\" " +
            $"--build-dir \"{Path.Combine(_engineRoot, "build")}\" --config Release " +
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
