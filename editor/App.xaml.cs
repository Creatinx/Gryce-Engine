using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using GryceEngine.Editor.Views;
using System.Windows;

namespace GryceEngine.Editor;

public partial class App : Application
{
    public static EngineService Engine { get; private set; } = null!;
    public static EditorViewModel EditorVM { get; private set; } = null!;

    public App()
    {
        // 捕获所有首次异常（含被处理/吞掉的），便于定位"干净退出"的触发点。
        AppDomain.CurrentDomain.FirstChanceException += (_, e) =>
        {
            try
            {
                WriteCrashLog(e.Exception, "FirstChance");
            }
            catch
            {
            }
        };
        // 未处理异常落盘（编辑器崩溃时留证据），同时弹窗提示而不是无声闪退。
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            WriteCrashLog(e.ExceptionObject as Exception ?? new Exception("unknown unhandled exception"),
                          "AppDomain.UnhandledException");
        };
        // 编辑器内未处理异常：弹窗提示而不是无声闪退（保留控制台日志路径）。
        DispatcherUnhandledException += (_, e) =>
        {
            WriteCrashLog(e.Exception, "DispatcherUnhandledException");
            try
            {
                MessageBox.Show($"Gryce Engine Editor error:\n{e.Exception.Message}",
                                "Editor Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            catch
            {
                // 异常处理本身失败时不阻塞退出流程
            }
            e.Handled = true;
        };
    }

    private static void WriteCrashLog(Exception ex, string source)
    {
        try
        {
            string path = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "editor_crash.log");
            System.IO.File.AppendAllText(path,
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] [{source}] {ex}\r\n");
        }
        catch
        {
            // 日志写入失败不影响主流程
        }
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        try
        {
            StartupCore(e);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Gryce Engine Editor failed to start:\n{ex.Message}",
                            "Startup Error", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown();
        }
    }

    private void StartupCore(StartupEventArgs e)
    {
        var settings = Services.EditorSettingsService.Load();

        Engine = new EngineService();

        // 恢复上次打开的项目；没有则回退到示例项目（examples/3dtest）。
        string startRoot = !string.IsNullOrWhiteSpace(settings.LastProject) &&
                           System.IO.Directory.Exists(settings.LastProject)
            ? settings.LastProject
            : "";
        // 首次启动（无上次项目）时显示欢迎页：新建/打开/最近项目。
        if (string.IsNullOrEmpty(startRoot))
        {
            startRoot = Views.WelcomeWindow.PickProject() ?? "";
        }
        Engine.Initialize(startRoot);

        // Restore persisted editor settings (language must be applied before the
        // ViewModel is created so localized VM strings use the right language).
        Engine.ReloadEditorSettings();
        if (settings.Language == "zh")
            Services.LocalizationService.Instance.Language = EditorLanguage.Chinese;

        // The ViewModel queries the C API (registered component types, callbacks),
        // so it must be constructed after the engine core is initialized.
        EditorVM = new EditorViewModel(Engine);

        // 编辑器默认停在 3D 标签：同步 Core 场景模式为 3D（2D/3D 场景槽各自独立）。
        // 空槽会自动创建/加载 scenes/scene_3d.gesc 缓冲文件。
        SceneAPI.GScene_SetMode(1);

        // 首次打开（空场景）时加载默认可编辑场景，保证用户一进来就能看到内容。
        if (EditorVM.EntityCount == 0)
        {
            int rc = SceneAPI.GScene_Load("res:/scenes/editor_default.gesc");
            if (rc != 0)
                EditorVM.AppendConsole("Failed to load default scene (editor_default.gesc).");
        }

        // Apply persisted theme globally (ThemeManager.Current.ApplicationTheme
        // switches the app-wide ThemeResources; per-window SetRequestedTheme is
        // only a local override and left the rest of the UI in the OS default).
        // 必须在 Show 之前应用：MainWindow.SourceInitialized 会用当前主题
        // 设置 DWM Mica 深色模式，先应用主题标题栏才不会停留在系统主题。
        iNKORE.UI.WPF.Modern.ThemeManager.Current.ApplicationTheme =
            settings.Theme == "Light"
                ? iNKORE.UI.WPF.Modern.ApplicationTheme.Light
                : iNKORE.UI.WPF.Modern.ApplicationTheme.Dark;

        var window = new MainWindow(EditorVM);
        // 欢迎页先于主窗口显示，WPF 会把欢迎页误设为 Application.MainWindow；
        // 显式指定主窗口，避免后续对话框以已关闭的欢迎页为 Owner。
        Application.Current.MainWindow = window;
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        WriteCrashLog(new Exception("Application exiting"), "OnExit " + Environment.StackTrace);
        EditorVM?.DetachCallbacks();
        Engine?.Shutdown();
        base.OnExit(e);
    }
}
