using GryceEngine.Editor.ViewModels;
using GryceEngine.Editor.Services;
using iNKORE.UI.WPF.Modern;
using System.Windows;

namespace GryceEngine.Editor.Views;

public partial class SettingsWindow : Window
{
    private readonly SettingsViewModel _viewModel;

    public SettingsWindow()
    {
        _viewModel = new SettingsViewModel();
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
        }

        // Persist language + theme so they survive restarts.
        Services.EditorSettingsService.Save(
            LocalizationService.Instance.LanguageCode,
            _viewModel.IsDarkTheme ? "Dark" : "Light");

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
