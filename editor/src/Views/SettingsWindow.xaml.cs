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

        // Apply theme to the main window
        if (Application.Current.MainWindow is Window mainWindow)
        {
            ThemeManager.SetRequestedTheme(mainWindow,
                _viewModel.IsDarkTheme ? ElementTheme.Dark : ElementTheme.Light);

            // Apply Mica backdrop to the editor window.
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
            _viewModel.MicaBackdrop);

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
