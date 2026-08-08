using GryceEngine.Editor.ViewModels;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class ProjectView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;

    private static readonly string ProjectRoot = ResolveProjectRoot();

    // 在文件浏览器中隐藏的顶层目录，避免把 build/out 等构建产物暴露给用户
    private static readonly HashSet<string> IgnoredTopDirs = new()
    {
        "build", "bin", "obj", "out", "x64", ".git", "deps_cache", ".idea", "__pycache__"
    };

    private string _currentPath = "";
    private string? _selectedFilePath;
    private List<FileItem> _allItems = new();

    // 从可执行文件目录向上追溯，定位引擎项目根目录（以此作为文件浏览器根）
    private static string ResolveProjectRoot()
    {
        var dir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")) ||
                Directory.Exists(Path.Combine(dir.FullName, "core")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return AppDomain.CurrentDomain.BaseDirectory;
    }

    private static bool IsIgnoredDir(string fullPath)
    {
        return IsIgnoredDirName(Path.GetFileName(fullPath));
    }

    private static bool IsIgnoredDirName(string name)
    {
        return name.StartsWith(".") || name.StartsWith("__") || IgnoredTopDirs.Contains(name);
    }

    public ProjectView()
    {
        InitializeComponent();
        Loaded += (_, _) => RefreshProjectTree();
    }

    private void OnRefreshClick(object sender, RoutedEventArgs e)
    {
        RefreshProjectTree();
    }

    private void RefreshProjectTree()
    {
        FolderTree.Items.Clear();
        if (!Directory.Exists(ProjectRoot))
        {
            FolderTree.Items.Add(new TreeViewItem { Header = "(Project folder not found)" });
            StatusText.Text = "Project folder not found";
            return;
        }

        var rootItem = CreateFolderTreeNode(ProjectRoot, Path.GetFileName(ProjectRoot));
        rootItem.IsExpanded = true;
        FolderTree.Items.Add(rootItem);

        // Select root and show its contents
        _currentPath = ProjectRoot;
        PathLabel.Text = Path.GetFileName(ProjectRoot);
        RefreshFileList(ProjectRoot);
        StatusText.Text = $"Project: {ProjectRoot}";
    }

    private static TreeViewItem CreateFolderTreeNode(string path, string name)
    {
        var item = new TreeViewItem
        {
            Header = CreateFolderHeader(name),
            Tag = path
        };

        // Add a placeholder to enable expand arrow
        item.Items.Add(new TreeViewItem { Header = "..." });

        item.Expanded += (_, _) =>
        {
            item.Items.Clear();
            try
            {
                foreach (var dir in Directory.GetDirectories(path))
                {
                    var dirName = Path.GetFileName(dir);
                    if (IsIgnoredDirName(dirName)) continue;
                    item.Items.Add(CreateFolderTreeNode(dir, dirName));
                }
            }
            catch { /* ignore */ }
        };

        return item;
    }

    /// <summary>Folder tree node header: Fluent folder glyph + name.</summary>
    private static object CreateFolderHeader(string name)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal };
        panel.Children.Add(new TextBlock
        {
            Text = "\uE8B7",
            FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
            FontSize = 12,
            Foreground = new SolidColorBrush(Color.FromRgb(0xDC, 0xB6, 0x7A)),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 6, 0)
        });
        panel.Children.Add(new TextBlock
        {
            Text = name,
            VerticalAlignment = VerticalAlignment.Center
        });
        return panel;
    }

    private void OnFolderTreeSelectionChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (e.NewValue is TreeViewItem item && item.Tag is string path)
        {
            _currentPath = path;
            UpdatePathLabel(path);
            RefreshFileList(path);
        }
    }

    private void UpdatePathLabel(string path)
    {
        if (path == ProjectRoot)
        {
            PathLabel.Text = Path.GetFileName(ProjectRoot);
            return;
        }
        var relative = path.Replace(ProjectRoot, "").TrimStart(Path.DirectorySeparatorChar);
        PathLabel.Text = Path.GetFileName(ProjectRoot) + "/" + relative.Replace('\\', '/');
    }

    private void RefreshFileList(string path)
    {
        _allItems = new List<FileItem>();

        try
        {
            // Add parent directory
            if (path != ProjectRoot)
            {
                _allItems.Add(new FileItem
                {
                    Name = "..",
                    Icon = "\uE8B7",
                    IconColor = new SolidColorBrush(Color.FromRgb(0xDC, 0xB6, 0x7A)),
                    IsDirectory = true,
                    Path = Path.GetDirectoryName(path) ?? ProjectRoot
                });
            }

            // Add folders (sorted alphabetically)
            var folders = new List<FileItem>();
            foreach (var dir in Directory.GetDirectories(path))
            {
                var dirName = Path.GetFileName(dir);
                if (IsIgnoredDirName(dirName)) continue;
                folders.Add(new FileItem
                {
                    Name = dirName,
                    Icon = "\uE8B7",
                    IconColor = new SolidColorBrush(Color.FromRgb(0xDC, 0xB6, 0x7A)),
                    IsDirectory = true,
                    Path = dir
                });
            }
            folders.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
            _allItems.AddRange(folders);

            // Add files (sorted alphabetically)
            var files = new List<FileItem>();
            foreach (var file in Directory.GetFiles(path))
            {
                var fileName = Path.GetFileName(file);
                if (fileName.EndsWith(".meta")) continue;
                var ext = Path.GetExtension(fileName).ToLowerInvariant();
                files.Add(new FileItem
                {
                    Name = fileName,
                    Icon = GetFileIcon(ext),
                    IconColor = GetFileColor(ext),
                    IsDirectory = false,
                    Path = file
                });
            }
            files.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
            _allItems.AddRange(files);
        }
        catch { /* ignore */ }

        UpdateItemCountAndView();
    }

    private void UpdateItemCountAndView()
    {
        int folderCount = _allItems.Count(f => f.IsDirectory);
        int fileCount = _allItems.Count(f => !f.IsDirectory);
        ItemCount.Text = $"{_allItems.Count} items ({folderCount} folders, {fileCount} files)";

        // Apply search filter
        ApplySearchFilter();

        // Show/hide empty state
        EmptyState.Visibility = _allItems.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ApplySearchFilter()
    {
        var searchText = SearchBox.Text?.Trim() ?? "";
        if (string.IsNullOrWhiteSpace(searchText))
        {
            FileListBox.ItemsSource = _allItems;
        }
        else
        {
            var filtered = _allItems
                .Where(item => item.Name.IndexOf(searchText, StringComparison.OrdinalIgnoreCase) >= 0)
                .ToList();
            FileListBox.ItemsSource = filtered;
        }
    }

    private static string GetFileIcon(string ext)
    {
        return ext switch
        {
            ".gesc" => "\uE7C1",
            ".obj" or ".fbx" or ".gltf" or ".glb" => "\uE8B8",
            ".png" or ".jpg" or ".jpeg" or ".dds" or ".bmp" => "\uE8B9",
            ".gmat" => "\uE791",
            ".gimport" => "\uE8A5",
            ".wav" or ".ogg" or ".mp3" => "\uE8D6",
            ".glsl" or ".vert" or ".frag" => "\uE943",
            ".cs" or ".cpp" or ".h" or ".hpp" => "\uE943",
            _ => "\uE8A5",
        };
    }

    private static SolidColorBrush GetFileColor(string ext)
    {
        var color = ext switch
        {
            ".gesc" => Color.FromRgb(0x80, 0xC0, 0xFF),
            ".obj" or ".fbx" or ".gltf" or ".glb" => Color.FromRgb(0x8C, 0xCB, 0xFF),
            ".png" or ".jpg" or ".jpeg" or ".dds" or ".bmp" => Color.FromRgb(0xFF, 0x8C, 0x8C),
            ".gmat" => Color.FromRgb(0x8C, 0xFF, 0x8C),
            ".gimport" => Color.FromRgb(0xDC, 0xB6, 0x7A),
            ".wav" or ".ogg" or ".mp3" => Color.FromRgb(0xFF, 0xCC, 0x8C),
            ".glsl" or ".vert" or ".frag" => Color.FromRgb(0xC0, 0x80, 0xFF),
            ".cs" or ".cpp" or ".h" or ".hpp" => Color.FromRgb(0x80, 0xC0, 0xFF),
            _ => Color.FromRgb(0xAA, 0xAA, 0xAA),
        };
        return new SolidColorBrush(color);
    }

    private void OnFileSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (FileListBox.SelectedItem is FileItem item)
        {
            _selectedFilePath = item.Path;
            StatusText.Text = item.IsDirectory
                ? $"Folder: {item.Name}"
                : $"File: {item.Name} ({Path.GetExtension(item.Name)})";
        }
    }

    private void OnFileDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (FileListBox.SelectedItem is not FileItem item) return;

        if (item.IsDirectory)
        {
            _currentPath = item.Path;
            UpdatePathLabel(item.Path);
            RefreshFileList(item.Path);
            return;
        }

        var ext = Path.GetExtension(item.Name).ToLowerInvariant();
        if (ext == ".gesc")
        {
            int result = Native.SceneAPI.GScene_Load(item.Path);
            if (result == 0)
                VM?.AppendConsole($"Scene loaded: {item.Path}");
            else
                VM?.AppendConsole($"Failed to load scene: {item.Path}");
        }
        else
        {
            VM?.AppendConsole($"Opened: {item.Path}");
        }
    }

    private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
    {
        ClearSearchBtn.Visibility = string.IsNullOrEmpty(SearchBox.Text)
            ? Visibility.Collapsed : Visibility.Visible;
        ApplySearchFilter();
    }

    private void OnClearSearchClick(object sender, RoutedEventArgs e)
    {
        SearchBox.Text = string.Empty;
    }

    private void OnNewFolderClick(object sender, RoutedEventArgs e)
    {
        var dialog = new InputDialog("New Folder", "Enter folder name:", "NewFolder");
        dialog.Owner = Window.GetWindow(this);
        if (dialog.ShowDialog() == true && !string.IsNullOrWhiteSpace(dialog.InputText))
        {
            try
            {
                var newPath = Path.Combine(_currentPath, dialog.InputText);
                Directory.CreateDirectory(newPath);
                VM?.AppendConsole($"Created folder: {dialog.InputText}");
                RefreshFileList(_currentPath);
            }
            catch (Exception ex)
            {
                VM?.AppendConsole($"Failed to create folder: {ex.Message}");
            }
        }
    }

    private void OnRenameClick(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_selectedFilePath)) return;
        var currentName = Path.GetFileName(_selectedFilePath);
        var dialog = new InputDialog("Rename", "Enter new name:", currentName);
        dialog.Owner = Window.GetWindow(this);
        if (dialog.ShowDialog() == true && !string.IsNullOrWhiteSpace(dialog.InputText))
        {
            try
            {
                var parentDir = Path.GetDirectoryName(_selectedFilePath) ?? _currentPath;
                var newPath = Path.Combine(parentDir, dialog.InputText);
                if (Directory.Exists(_selectedFilePath))
                    Directory.Move(_selectedFilePath, newPath);
                else if (File.Exists(_selectedFilePath))
                    File.Move(_selectedFilePath, newPath);
                VM?.AppendConsole($"Renamed: {currentName} -> {dialog.InputText}");
                RefreshFileList(_currentPath);
            }
            catch (Exception ex)
            {
                VM?.AppendConsole($"Failed to rename: {ex.Message}");
            }
        }
    }

    private void OnDeleteClick(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_selectedFilePath)) return;
        var name = Path.GetFileName(_selectedFilePath);
        var result = MessageBox.Show(
            $"Are you sure you want to delete '{name}'?",
            "Confirm Delete",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);

        if (result == MessageBoxResult.Yes)
        {
            try
            {
                if (Directory.Exists(_selectedFilePath))
                    Directory.Delete(_selectedFilePath, true);
                else if (File.Exists(_selectedFilePath))
                    File.Delete(_selectedFilePath);
                VM?.AppendConsole($"Deleted: {name}");
                _selectedFilePath = null;
                RefreshFileList(_currentPath);
            }
            catch (Exception ex)
            {
                VM?.AppendConsole($"Failed to delete: {ex.Message}");
            }
        }
    }

    private void OnCopyPathClick(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_selectedFilePath)) return;
        Clipboard.SetText(_selectedFilePath);
        VM?.AppendConsole($"Copied path: {_selectedFilePath}");
    }

    private void OnOpenInExplorerClick(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_selectedFilePath)) return;
        try
        {
            var targetPath = Directory.Exists(_selectedFilePath) ? _selectedFilePath : Path.GetDirectoryName(_selectedFilePath);
            if (targetPath != null)
                Process.Start("explorer.exe", targetPath);
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"Failed to open explorer: {ex.Message}");
        }
    }
}

/// <summary>Represents a file or folder in the project browser.</summary>
public class FileItem : INotifyPropertyChanged
{
    public string Name { get; set; } = "";
    public string Icon { get; set; } = "\uE8A5";
    public Brush IconColor { get; set; } = Brushes.Gray;
    public bool IsDirectory { get; set; }
    public string Path { get; set; } = "";

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
