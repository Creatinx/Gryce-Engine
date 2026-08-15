using GryceEngine.Editor.Services;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace GryceEngine.Editor.Views;

/// <summary>
/// 首启欢迎页（项目选择器）：新建项目 / 打开项目 / 最近项目 / 打开示例。
/// 在没有"上次打开的项目"时于启动前展示，返回选中的项目根目录
/// （空字符串 = 示例项目 examples/3dtest，null = 取消并回退默认）。
/// </summary>
public partial class WelcomeWindow : Window
{
    /// <summary>用户选中的项目根目录；"" 表示示例项目；null 表示取消。</summary>
    public string? SelectedRoot { get; private set; }

    public WelcomeWindow()
    {
        InitializeComponent();
        var recents = EditorSettingsService.GetRecentProjects();
        RecentList.ItemsSource = recents;
        bool hasRecents = recents.Count > 0;
        RecentHint.Visibility = hasRecents ? Visibility.Collapsed : Visibility.Visible;
        if (hasRecents) RecentList.SelectedIndex = 0;
    }

    /// <summary>显示欢迎页并返回选择结果（模态；调用前引擎未初始化）。</summary>
    public static string? PickProject()
    {
        var win = new WelcomeWindow();
        win.ShowDialog();
        return win.SelectedRoot;
    }

    private void OnNewProjectClick(object sender, RoutedEventArgs e)
    {
        var dialog = new NewProjectDialog("");
        if (ModalDialog.Show(dialog, this) == true && dialog.CreatedProjectRoot != null)
        {
            SelectedRoot = dialog.CreatedProjectRoot;
            DialogResult = true;
        }
    }

    private void OnOpenProjectClick(object sender, RoutedEventArgs e)
    {
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = LocalizationService.Instance.T("menu.open_project"),
            ShowNewFolderButton = false
        };
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            SelectedRoot = dialog.SelectedPath;
            DialogResult = true;
        }
    }

    private void OnOpenExampleClick(object sender, RoutedEventArgs e)
    {
        // 空字符串 = 走 Engine.Initialize("") 的示例项目回退（examples/3dtest）
        SelectedRoot = "";
        DialogResult = true;
    }

    private void OnRecentSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        BtnOpenRecent.IsEnabled = RecentList.SelectedItem is string;
    }

    private void OnRecentDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (RecentList.SelectedItem is string path) OpenRecent(path);
    }

    private void OnOpenRecentClick(object sender, RoutedEventArgs e)
    {
        if (RecentList.SelectedItem is string path) OpenRecent(path);
    }

    private void OpenRecent(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !Directory.Exists(path))
        {
            MessageBox.Show(this, LocalizationService.Instance.T("welcome.missing_recent"),
                            LocalizationService.Instance.T("welcome.title"),
                            MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        SelectedRoot = path;
        DialogResult = true;
    }

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }
}
