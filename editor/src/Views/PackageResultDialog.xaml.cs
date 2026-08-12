using GryceEngine.Editor.Services;
using System;
using System.Diagnostics;
using System.IO;
using System.Windows;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Editor-styled popup shown after GryceGC finishes: success message with the
/// output path, an "Open Folder" action, and OK / Cancel buttons.
/// </summary>
public partial class PackageResultDialog : Window
{
    private readonly string? _outputDir;

    public PackageResultDialog(bool success, string? outputDir, string message)
    {
        InitializeComponent();
        _outputDir = outputDir;
        Title = LocalizationService.Instance.T(
            success ? "package.success_title" : "package.failed_title");
        MessageText.Text = message;
        PathText.Text = outputDir ?? string.Empty;
        IconText.Text = success ? "\uE73E" : "\uE783";
        OpenFolderButton.Visibility =
            success && !string.IsNullOrEmpty(outputDir) && Directory.Exists(outputDir)
                ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnOpenFolderClick(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_outputDir) || !Directory.Exists(_outputDir)) return;
        try
        {
            // Start explorer.exe directly (not through ShellExecute): the
            // shell can take a long time to answer when Explorer is busy, and
            // this opens the folder window immediately.
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{_outputDir}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true
            });
        }
        catch { /* ignore */ }
    }

    private void OnOkClick(object sender, RoutedEventArgs e) => DialogResult = true;
    private void OnCancelClick(object sender, RoutedEventArgs e) => DialogResult = false;
}
