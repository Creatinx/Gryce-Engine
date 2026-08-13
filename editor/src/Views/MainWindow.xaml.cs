using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using iNKORE.UI.WPF.Modern;
using Microsoft.Win32;
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class MainWindow
{
    private EditorViewModel? VM => DataContext as EditorViewModel;

    public MainWindow(EditorViewModel viewModel)
    {
        DataContext = viewModel;
        InitializeComponent();
        // 代码编辑器（Script 标签）激活时隐藏场景编辑器工具栏的变换/Gizmo 工具
        ViewportHost.ScriptModeChanged += scriptMode =>
            EditorToolbar.SetSceneToolsVisible(scriptMode);
        SourceInitialized += OnSourceInitialized;
        viewModel.RefreshHierarchy();
        viewModel.PropertyChanged += OnViewModelPropertyChanged;
        UpdatePlaybackState(viewModel);
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        // Apply the persisted Mica backdrop to the title bar + editor background.
        var settings = Services.EditorSettingsService.Load();
        if (settings.MicaBackdrop)
        {
            Services.MicaHelper.TryApplyMica(this);
        }
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(VM.IsPlaying) || e.PropertyName == nameof(VM.IsPaused))
        {
            UpdatePlaybackState(VM);
        }
    }

    private void UpdatePlaybackState(EditorViewModel? vm)
    {
        if (vm == null) return;
        if (vm.IsPlaying && vm.IsPaused)
            PlaybackStateText.Text = LocalizationService.Instance.T("status.paused");
        else if (vm.IsPlaying)
            PlaybackStateText.Text = LocalizationService.Instance.T("status.playing");
        else
            PlaybackStateText.Text = LocalizationService.Instance.T("status.editor");
    }

    // === Window Buttons ===

    private void OnMinimizeClick(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void OnMaximizeClick(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void OnCloseClick(object sender, RoutedEventArgs e) => Close();

    // === Playback Bar ===

    private void OnPlayClick(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        // Play and Pause are separate buttons: Play only starts/resumes.
        if (!VM.IsPlaying || VM.IsPaused) VM.Play();
    }

    private void OnPauseClick(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        // Play and Pause are separate buttons: Pause only pauses.
        if (VM.IsPlaying && !VM.IsPaused) VM.Pause();
    }

    private void OnStopClick(object sender, RoutedEventArgs e) => VM?.Stop();

    // === File Menu ===

    private void OnLoadSceneClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = "Load Scene",
            Filter = "Gryce Scene Files (*.gesc)|*.gesc|All Files (*.*)|*.*",
            DefaultExt = ".gesc"
        };
        if (dialog.ShowDialog() == true)
        {
            int result = SceneAPI.GScene_Load(dialog.FileName);
            if (result == 0)
            {
                VM?.AppendConsole($"Scene loaded: {dialog.FileName}");
                VM?.SetSceneName(System.IO.Path.GetFileNameWithoutExtension(dialog.FileName));
            }
            else
                VM?.AppendConsole($"Failed to load scene: {dialog.FileName}");
        }
    }

    private void OnOpenProjectClick(object sender, RoutedEventArgs e)
    {
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = "Select Game Project Root",
            ShowNewFolderButton = false
        };
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            App.Engine.ReloadProject(dialog.SelectedPath);
        }
    }

    private void OnNewProjectClick(object sender, RoutedEventArgs e)
    {
        var dialog = new NewProjectDialog(App.Engine.ProjectRoot);
        if (ModalDialog.Show(dialog, this) == true && dialog.CreatedProjectRoot != null)
        {
            App.Engine.ReloadProject(dialog.CreatedProjectRoot);
            // 加载脚手架生成的主场景，并让视图刷新到底层实体树。
            int rc = SceneAPI.GScene_Load("res:/scenes/main.gesc");
            VM?.AppendConsole(rc == 0
                ? $"Created project: {dialog.CreatedProjectRoot}"
                : $"Created project, but failed to load main scene: {dialog.CreatedProjectRoot}");
            VM?.RefreshHierarchy();
        }
    }

    private void OnImportAssetClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = "Import Asset",
            Filter = "3D Models (*.obj;*.fbx;*.gltf;*.glb)|*.obj;*.fbx;*.gltf;*.glb|Audio (*.wav;*.ogg;*.mp3;*.flac)|*.wav;*.ogg;*.mp3;*.flac|Textures (*.png;*.jpg;*.jpeg;*.dds)|*.png;*.jpg;*.jpeg;*.dds|All Files (*.*)|*.*",
            Multiselect = true
        };
        if (dialog.ShowDialog() == true)
        {
            foreach (var file in dialog.FileNames)
            {
                var handle = AssetAPI.GAsset_Import(file);
                if (handle != GAssetHandle.Null)
                    VM?.AppendConsole($"Imported: {file} -> handle {handle}");
                else
                    VM?.AppendConsole($"Failed to import: {file}");
            }
        }
    }

    private void OnPackageGameClick(object sender, RoutedEventArgs e)
    {
        try
        {
            string engineRoot = FindEngineRoot();
            string projectRoot = Services.EngineService.Current?.ProjectRoot ?? engineRoot;
            if (string.IsNullOrEmpty(engineRoot) || !File.Exists(Path.Combine(engineRoot, "CMakeLists.txt")))
            {
                VM?.AppendConsole("Package: engine root not found.");
                return;
            }

            // Packaging setup dialog: platform + game name + output directory.
            var dialog = new PackageDialog(engineRoot, projectRoot);
            dialog.Log += line =>
            {
                if (VM != null) VM.AppendConsole(line);
            };
            if (ModalDialog.Show(dialog, this) == true && dialog.Succeeded)
            {
                // Editor-styled success popup with OK / Cancel / Open Folder.
                string message = Services.LocalizationService.Instance.T("package.success_msg");
                var result = new PackageResultDialog(true, dialog.LastOutputDir, message);
                ModalDialog.Show(result, this);
            }
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"Package failed: {ex.Message}");
        }
    }

    private static string FindEngineRoot()
    {
        var dir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")))
                return dir.FullName;
            dir = dir.Parent;
        }
        return string.Empty;
    }

    private void OnExitClick(object sender, RoutedEventArgs e)
    {
        Application.Current.Shutdown();
    }

    private void OnCreateEntityClick(object sender, RoutedEventArgs e)
    {
        VM?.CreateEntityThenOpenComponentPicker();
    }

    // === View Menu ===

    private void OnRefreshViewClick(object sender, RoutedEventArgs e)
    {
        VM?.RefreshHierarchy();
        VM?.AppendConsole("View refreshed.");
    }

    // === Theme Switching ===

    private void OnThemeDarkClick(object sender, RoutedEventArgs e)
    {
        ApplyTheme(ElementTheme.Dark);
        if (VM != null) VM.CurrentTheme = "Dark";
    }

    private void OnThemeLightClick(object sender, RoutedEventArgs e)
    {
        ApplyTheme(ElementTheme.Light);
        if (VM != null) VM.CurrentTheme = "Light";
    }

    private static void ApplyTheme(ElementTheme theme)
    {
        if (Application.Current.MainWindow is Window mainWindow)
        {
            ThemeManager.SetRequestedTheme(mainWindow, theme);
        }
    }

    // === Settings & About ===

    private void OnSettingsClick(object sender, RoutedEventArgs e)
    {
        if (ModalDialog.Show(new SettingsWindow(), this) == true)
        {
            VM?.AppendConsole("Settings applied.");
        }
    }

    private void OnProjectSettingsClick(object sender, RoutedEventArgs e)
    {
        ModalDialog.Show(new ProjectSettingsWindow(), this);
    }

    private void OnAboutClick(object sender, RoutedEventArgs e)
    {
        // A real modal window: the previous in-window ContentDialog was covered
        // by the native GLFW viewport (WPF airspace).
        ModalDialog.Show(new AboutWindow(), this);
    }
}
