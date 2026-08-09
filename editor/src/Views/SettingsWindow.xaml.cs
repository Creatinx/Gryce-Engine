using GryceEngine.Editor.ViewModels;
using GryceEngine.Editor.Services;
using iNKORE.UI.WPF.Modern;
using System.Windows;
using System.Windows.Interop;

namespace GryceEngine.Editor.Views;

public partial class SettingsWindow : Window
{
    private readonly SettingsViewModel _viewModel;

    public SettingsWindow()
    {
        var saved = Services.EditorSettingsService.Load();
        _viewModel = new SettingsViewModel();
        _viewModel.IsDarkTheme = saved.Theme != "Light";
        _viewModel.IsLightTheme = saved.Theme == "Light";
        _viewModel.MicaBackdrop = saved.MicaBackdrop;
        _viewModel.AutoSaveInterval = saved.AutoSaveInterval;
        _viewModel.SelectedLanguageIndex =
            string.Equals(saved.Language, "zh", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
        _viewModel.ApplyRequested += OnApply;
        _viewModel.CancelRequested += OnCancel;
        DataContext = _viewModel;
        InitializeComponent();
    }

    private void OnApply()
    {
        // Apply language (enum order matches the Languages array in the ViewModel)
        LocalizationService.Instance.Language = (EditorLanguage)_viewModel.SelectedLanguageIndex;

        // Apply the theme globally so every window and control reloads its
        // DynamicResource brushes together (per-window SetRequestedTheme left
        // part of the UI on the OS default and mixed dark/light elements).
        ThemeManager.Current.ApplicationTheme =
            _viewModel.IsDarkTheme ? ApplicationTheme.Dark : ApplicationTheme.Light;

        // Apply Mica backdrop to the editor window.
        if (Application.Current.MainWindow is Window mainWindow)
        {
            var mainHwnd = new WindowInteropHelper(mainWindow).Handle;
            if (_viewModel.MicaBackdrop)
                Services.MicaHelper.TryApplyMica(mainHwnd);
            else
                Services.MicaHelper.TryRemoveMica(mainHwnd);
        }

        // Persist language + theme + backdrop so they survive restarts.
        Services.EditorSettingsService.Save(
            LocalizationService.Instance.LanguageCode,
            _viewModel.IsDarkTheme ? "Dark" : "Light",
            _viewModel.MicaBackdrop,
            _viewModel.AutoSaveInterval);

        // Apply auto-save interval to the engine service (minutes, 0 = off).
        App.Engine?.UpdateAutoSaveInterval(_viewModel.AutoSaveInterval);

        // Apply VSync setting to renderer
        try
        {
            Native.RenderAPI.GRender_SetVSync(_viewModel.VSyncEnabled);
        }
        catch { /* Renderer may not be initialized yet */ }

        DialogResult = true;
        Close();
    }

    private void OnCancel()
    {
        DialogResult = false;
        Close();
    }
}
