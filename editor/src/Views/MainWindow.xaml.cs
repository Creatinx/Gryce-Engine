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
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace GryceEngine.Editor.Views;

public partial class MainWindow
{
    private EditorViewModel? VM => DataContext as EditorViewModel;
    private readonly DispatcherTimer _toastTimer = new() { Interval = TimeSpan.FromSeconds(4) };
    private bool _shutdownRequested;

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
        viewModel.StatusToast += ShowToast;
        UpdatePlaybackState(viewModel);

        // 菜单显示 "Ctrl+Shift+N" 但此前从未绑定快捷键：补上。
        InputBindings.Add(new KeyBinding(
            new RelayCommand(() => OnNewProjectClick(this, new RoutedEventArgs())),
            Key.N, ModifierKeys.Control | ModifierKeys.Shift));
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        base.OnClosing(e);
        WriteEditorLog("MainWindow.OnClosing cancel=" + e.Cancel + "\r\n" + Environment.StackTrace);

        var engine = App.Engine;
        if (engine == null || !engine.IsInitialized || !engine.IsSceneDirty || engine.IsPlaying) return;

        var loc = LocalizationService.Instance;
        var result = MessageBox.Show(this,
            loc.T("confirm.unsaved_message"),
            loc.T("confirm.unsaved_title"),
            MessageBoxButton.YesNoCancel,
            MessageBoxImage.Warning);

        if (result == MessageBoxResult.Yes)
        {
            VM?.SaveScene();
        }
        else if (result == MessageBoxResult.Cancel)
        {
            e.Cancel = true;
        }
        // No = discard unsaved changes and close.
    }

    protected override void OnClosed(EventArgs e)
    {
        WriteEditorLog("MainWindow.OnClosed\r\n" + Environment.StackTrace);
        base.OnClosed(e);
        // OnExplicitShutdown 下主窗口关闭不会自动退出应用；
        // 主窗口是最后一个窗口，关闭即显式退出（延迟到关闭流程结束后，
        // 避免 File→Exit 已触发的 Shutdown 重入）。
        if (!_shutdownRequested && Application.Current != null)
        {
            _shutdownRequested = true;
            try
            {
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    try { Application.Current.Shutdown(); } catch { }
                }), DispatcherPriority.ContextIdle);
            }
            catch
            {
            }
        }
    }

    private static void WriteEditorLog(string message)
    {
        try
        {
            string path = System.IO.Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory, "editor_crash.log");
            System.IO.File.AppendAllText(path,
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {message}\r\n");
        }
        catch
        {
        }
    }

    /// <summary>Transient status toast: shows the message in the status bar and
    /// fades it out after a few seconds (errors use a warning color).</summary>
    private void ShowToast(string message, bool isError)
    {
        StatusToast.Text = message;
        StatusToast.Opacity = 1.0;
        StatusToast.BeginAnimation(OpacityProperty, null);
        StatusToast.Foreground = isError
            ? new SolidColorBrush(Color.FromRgb(0xE5, 0x48, 0x4D))
            : (Brush)FindResource("TextFillColorSecondaryBrush");

        _toastTimer.Stop();
        _toastTimer.Tick -= OnToastTick;
        _toastTimer.Tick += OnToastTick;
        _toastTimer.Start();
    }

    private void OnToastTick(object? sender, EventArgs e)
    {
        _toastTimer.Stop();
        StatusToast.BeginAnimation(OpacityProperty,
            new DoubleAnimation(1.0, 0.0, TimeSpan.FromMilliseconds(400)));
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
        // 播放按钮：点击播放，再点一下即暂停/继续切换。
        if (VM.IsPlaying) VM.Pause();
        else VM.Play();
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
                VM?.Notify($"Failed to load scene: {dialog.FileName}", true);
        }
    }

    private void OnOpenProjectClick(object sender, RoutedEventArgs e)
    {
        WriteEditorLog("OnOpenProjectClick: start");
        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = "Select Game Project Root",
            ShowNewFolderButton = false
        };
        WriteEditorLog("OnOpenProjectClick: showing FolderBrowserDialog");
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            WriteEditorLog("OnOpenProjectClick: selected " + dialog.SelectedPath);
            App.Engine.ReloadProject(dialog.SelectedPath);
            WriteEditorLog("OnOpenProjectClick: ReloadProject returned");
        }
        WriteEditorLog("OnOpenProjectClick: end");
    }

    private void OnNewProjectClick(object sender, RoutedEventArgs e)
    {
        WriteEditorLog("OnNewProjectClick: start");
        var dialog = new NewProjectDialog(App.Engine.ProjectRoot);
        WriteEditorLog("OnNewProjectClick: showing NewProjectDialog");
        if (ModalDialog.Show(dialog, this) == true && dialog.CreatedProjectRoot != null)
        {
            WriteEditorLog("OnNewProjectClick: created " + dialog.CreatedProjectRoot);
            App.Engine.ReloadProject(dialog.CreatedProjectRoot);
            WriteEditorLog("OnNewProjectClick: ReloadProject returned");
            // 加载脚手架生成的主场景，并让视图刷新到底层实体树。
            int rc = SceneAPI.GScene_Load("res:/scenes/main.gesc");
            if (rc == 0)
            {
                // 把当前场景保存为主场景，确保 main.gesc 与
                // project_settings.json 的 main_scene 指向一致。
                SceneAPI.GScene_Save("res:/scenes/main.gesc");
            }
            WriteEditorLog("OnNewProjectClick: GScene_Load rc=" + rc);
            VM?.AppendConsole(rc == 0
                ? $"Created project: {dialog.CreatedProjectRoot}"
                : $"Created project, but failed to load main scene: {dialog.CreatedProjectRoot}");
            VM?.RefreshHierarchy();
        }
        WriteEditorLog("OnNewProjectClick: end");
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
                    VM?.Notify($"Failed to import: {file}", true);
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
            VM?.Notify($"Package failed: {ex.Message}", true);
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
