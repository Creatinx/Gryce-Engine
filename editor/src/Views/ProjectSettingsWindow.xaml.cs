using GryceEngine.Editor.ViewModels;
using System.Windows;

namespace GryceEngine.Editor.Views;

public partial class ProjectSettingsWindow : Window
{
    private readonly ProjectSettingsViewModel _viewModel;

    public ProjectSettingsWindow()
    {
        _viewModel = new ProjectSettingsViewModel();
        _viewModel.ApplyRequested += () => DialogResult = true;
        _viewModel.CancelRequested += () => DialogResult = false;
        DataContext = _viewModel;
        InitializeComponent();
    }
}
