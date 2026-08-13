using GryceEngine.Editor.Services;
using System;
using System.IO;
using System.Windows;

namespace GryceEngine.Editor.Views;

/// <summary>
/// 新建项目对话框（参考 Godot 项目管理器）：输入项目名 + 父目录 + 模板类型，
/// 创建目录脚手架后切换编辑器到新项目。
/// </summary>
public partial class NewProjectDialog : Window
{
    /// <summary>创建成功后新项目的根目录（由脚手架确定的完整路径）。</summary>
    public string? CreatedProjectRoot { get; private set; }

    public NewProjectDialog(string currentRoot)
    {
        InitializeComponent();
        // 默认父目录：当前项目所在目录的上一级（引擎 examples 或用户目录）。
        string? parent = null;
        if (Directory.Exists(currentRoot))
        {
            var dir = new DirectoryInfo(currentRoot);
            if (dir.Parent != null) parent = dir.Parent.FullName;
        }
        if (string.IsNullOrEmpty(parent))
            parent = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        ParentDirBox.Text = parent;
        UpdateTarget();
    }

    private bool Is3D => Template3D.IsChecked == true;

    private void OnNameChanged(object sender, RoutedEventArgs e) => UpdateTarget();

    private void OnBrowseClick(object sender, RoutedEventArgs e)
    {
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = LocalizationService.Instance.T("new_project.parent_dir"),
            ShowNewFolderButton = true,
            SelectedPath = Directory.Exists(ParentDirBox.Text) ? ParentDirBox.Text : ""
        };
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            ParentDirBox.Text = dialog.SelectedPath;
            UpdateTarget();
        }
    }

    private void UpdateTarget()
    {
        string name = NameBox.Text.Trim();
        string parent = ParentDirBox.Text.Trim();
        string full = Path.Combine(parent, name);
        TargetText.Text = LocalizationService.Instance.T("new_project.target") + "  " + full;

        bool nameOk = !string.IsNullOrWhiteSpace(name);
        bool parentOk = !string.IsNullOrWhiteSpace(parent) && Directory.Exists(parent);
        CreateButton.IsEnabled = nameOk && parentOk;
    }

    private void OnCancelClick(object sender, RoutedEventArgs e) => DialogResult = false;

    private void OnCreateClick(object sender, RoutedEventArgs e)
    {
        string? error = ProjectScaffolder.Create(NameBox.Text, ParentDirBox.Text.Trim(), Is3D);
        if (error != null)
        {
            string msg = error;
            // 允许脚手架返回带冒号的附加信息（如 IO 异常），仅展示本地化主文案。
            string key = msg;
            int colon = msg.IndexOf(':');
            if (colon > 0) key = msg.Substring(0, colon);
            StatusText.Text = LocalizationService.Instance.T(key);
            StatusText.Visibility = Visibility.Visible;
            return;
        }

        CreatedProjectRoot = Path.Combine(ParentDirBox.Text.Trim(), NameBox.Text.Trim());
        DialogResult = true;
    }
}