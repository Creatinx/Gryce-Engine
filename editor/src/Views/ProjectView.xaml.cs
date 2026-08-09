using GryceEngine.Editor.Services;
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
            FolderTree.Items.Add(new TreeViewItem
            {
                Header = LocalizationService.Instance.T("project.folder_not_found")
            });
            StatusText.Text = LocalizationService.Instance.T("project.folder_not_found");
            return;
        }

        var rootItem = CreateFolderTreeNode(ProjectRoot,
            LocalizationService.Instance.T("project.root"));
        rootItem.IsExpanded = true;
        FolderTree.Items.Add(rootItem);

        // Select root and show its contents
        _currentPath = ProjectRoot;
        UpdatePathLabel(ProjectRoot);
        RefreshFileList(ProjectRoot);
        StatusText.Text = string.Format(
            LocalizationService.Instance.T("project.project_root"), ProjectRoot);
    }

    private static TreeViewItem CreateFolderTreeNode(string path, string name)
    {
        var item = new TreeViewItem
        {
            Header = CreateFolderHeader(name),
            Tag = path,
            Style = CompactTreeItemStyle()
        };

        // Add a placeholder to enable expand arrow
        item.Items.Add(new TreeViewItem { Header = "...", Style = CompactTreeItemStyle() });

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

    /// <summary>
    /// The compact tree style must be assigned as a local Style: the iNKORE
    /// theme's TreeViewItem look is not overridden by ItemContainerStyle alone,
    /// so explicitly-created containers get it directly here.
    /// </summary>
    private static Style CompactTreeItemStyle()
    {
        return (Style)System.Windows.Application.Current.FindResource("CompactTreeItemStyle");
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
        BtnUp.IsEnabled = path != ProjectRoot;
        if (path == ProjectRoot)
        {
            PathLabel.Text = LocalizationService.Instance.T("project.root");
            return;
        }
        var relative = path.Replace(ProjectRoot, "").TrimStart(Path.DirectorySeparatorChar);
        PathLabel.Text = LocalizationService.Instance.T("project.root") + "/" +
                         relative.Replace('\\', '/');
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
        ItemCount.Text = string.Format(
            LocalizationService.Instance.T("project.items_count"),
            _allItems.Count, folderCount, fileCount);

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
            ".obj" or ".fbx" or ".gltf" or ".glb" or ".dae" or ".blend" or ".3ds" => "\uE8B8",
            ".png" or ".jpg" or ".jpeg" or ".dds" or ".bmp" or ".tga" or ".svg" or ".webp" => "\uE8B9",
            ".gmat" => "\uE791",
            ".gimport" or ".meta" => "\uE8A5",
            ".wav" or ".ogg" or ".mp3" or ".flac" or ".aac" or ".wma" => "\uE8D6",
            ".mp4" or ".avi" or ".mov" or ".mkv" or ".webm" => "\uE714",
            ".glsl" or ".vert" or ".frag" or ".geom" or ".shader" or ".hlsl" => "\uE943",
            ".cs" or ".cpp" or ".cc" or ".cxx" or ".h" or ".hpp" or ".hxx" or ".c" => "\uE943",
            ".py" or ".js" or ".ts" or ".lua" or ".gd" => "\uE7FC",
            ".json" or ".xml" or ".yaml" or ".yml" or ".toml" or ".ini" or ".cfg" or ".csv" => "\uE8A5",
            ".md" or ".txt" or ".log" => "\uE8A5",
            ".ttf" or ".otf" => "\uE8D2",
            ".zip" or ".rar" or ".7z" or ".tar" or ".gz" => "\uE7B8",
            ".exe" or ".dll" or ".lib" or ".so" => "\uE950",
            ".sln" or ".csproj" or ".vcxproj" or ".slnx" or ".cmake" => "\uE943",
            ".prefab" or ".tscn" or ".tres" => "\uE8B8",
            ".scene" => "\uE7C1",
            ".html" or ".htm" or ".css" => "\uE774",
            _ => "\uE8A5",
        };
    }

    private static SolidColorBrush GetFileColor(string ext)
    {
        var color = ext switch
        {
            ".gesc" => Color.FromRgb(0x80, 0xC0, 0xFF),
            ".obj" or ".fbx" or ".gltf" or ".glb" or ".dae" or ".blend" or ".3ds" => Color.FromRgb(0x8C, 0xCB, 0xFF),
            ".png" or ".jpg" or ".jpeg" or ".dds" or ".bmp" or ".tga" or ".svg" or ".webp" => Color.FromRgb(0xFF, 0x8C, 0x8C),
            ".gmat" => Color.FromRgb(0x8C, 0xFF, 0x8C),
            ".gimport" or ".meta" => Color.FromRgb(0xDC, 0xB6, 0x7A),
            ".wav" or ".ogg" or ".mp3" or ".flac" or ".aac" or ".wma" => Color.FromRgb(0xFF, 0xCC, 0x8C),
            ".mp4" or ".avi" or ".mov" or ".mkv" or ".webm" => Color.FromRgb(0xFF, 0xAA, 0xDD),
            ".glsl" or ".vert" or ".frag" or ".geom" or ".shader" or ".hlsl" => Color.FromRgb(0xC0, 0x80, 0xFF),
            ".cs" or ".cpp" or ".cc" or ".cxx" or ".h" or ".hpp" or ".hxx" or ".c" => Color.FromRgb(0x80, 0xC0, 0xFF),
            ".py" or ".js" or ".ts" or ".lua" or ".gd" => Color.FromRgb(0xCC, 0xCC, 0x66),
            ".json" or ".xml" or ".yaml" or ".yml" or ".toml" or ".ini" or ".cfg" or ".csv" => Color.FromRgb(0xAA, 0xAA, 0xAA),
            ".md" or ".txt" or ".log" => Color.FromRgb(0xBB, 0xBB, 0xBB),
            ".ttf" or ".otf" => Color.FromRgb(0x9C, 0xD8, 0xFF),
            ".zip" or ".rar" or ".7z" or ".tar" or ".gz" => Color.FromRgb(0xFF, 0xD0, 0x66),
            ".exe" or ".dll" or ".lib" or ".so" => Color.FromRgb(0x8C, 0xFF, 0xD0),
            ".sln" or ".csproj" or ".vcxproj" or ".slnx" or ".cmake" => Color.FromRgb(0xC0, 0x80, 0xFF),
            ".prefab" or ".tscn" or ".tres" => Color.FromRgb(0x8C, 0xCB, 0xFF),
            ".scene" => Color.FromRgb(0x80, 0xC0, 0xFF),
            ".html" or ".htm" or ".css" => Color.FromRgb(0xFF, 0x8C, 0x66),
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
                ? string.Format(LocalizationService.Instance.T("project.folder"), item.Name)
                : string.Format(LocalizationService.Instance.T("project.file"),
                    item.Name, Path.GetExtension(item.Name));
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

    private void OnUpClick(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_currentPath) || _currentPath == ProjectRoot) return;
        var parent = Path.GetDirectoryName(_currentPath);
        if (string.IsNullOrEmpty(parent)) return;
        _currentPath = parent;
        UpdatePathLabel(parent);
        RefreshFileList(parent);
    }

    private void OnNewFolderClick(object sender, RoutedEventArgs e)
    {
        var dialog = new InputDialog("New Folder", "Enter folder name:", "NewFolder");
        if (ModalDialog.Show(dialog, Window.GetWindow(this)) == true &&
            !string.IsNullOrWhiteSpace(dialog.InputText))
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
        if (ModalDialog.Show(dialog, Window.GetWindow(this)) == true &&
            !string.IsNullOrWhiteSpace(dialog.InputText))
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
