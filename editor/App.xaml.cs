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

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        Engine = new EngineService();

        // Initialize engine core (empty root => auto-detect examples/3dtest project)
        Engine.Initialize("");

        // Restore persisted editor settings (language must be applied before the
        // ViewModel is created so localized VM strings use the right language).
        var settings = Services.EditorSettingsService.Load();
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

        var window = new MainWindow(EditorVM);
        window.Show();

        // Apply persisted theme globally (ThemeManager.Current.ApplicationTheme
        // switches the app-wide ThemeResources; per-window SetRequestedTheme is
        // only a local override and left the rest of the UI in the OS default).
        iNKORE.UI.WPF.Modern.ThemeManager.Current.ApplicationTheme =
            settings.Theme == "Light"
                ? iNKORE.UI.WPF.Modern.ApplicationTheme.Light
                : iNKORE.UI.WPF.Modern.ApplicationTheme.Dark;
    }

    protected override void OnExit(ExitEventArgs e)
    {
        EditorVM?.DetachCallbacks();
        Engine?.Shutdown();
        base.OnExit(e);
    }
}
