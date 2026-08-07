using System.ComponentModel;
using GryceEngine.Editor.Services;
using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace GryceEngine.Editor.ViewModels;

/// <summary>ViewModel for the Settings dialog.</summary>
public class SettingsViewModel : INotifyPropertyChanged
{
    // Theme
    private bool _isDarkTheme = true;
    private bool _isLightTheme;
    public bool IsDarkTheme
    {
        get => _isDarkTheme;
        set { _isDarkTheme = value; OnPropertyChanged(); OnPropertyChanged(nameof(IsLightTheme)); }
    }
    public bool IsLightTheme
    {
        get => _isLightTheme;
        set { _isLightTheme = value; OnPropertyChanged(); OnPropertyChanged(nameof(IsDarkTheme)); }
    }

    // Editor
    private bool _vSyncEnabled = true;
    public bool VSyncEnabled
    {
        get => _vSyncEnabled;
        set { _vSyncEnabled = value; OnPropertyChanged(); }
    }

    private int _autoSaveInterval = 300;
    public int AutoSaveInterval
    {
        get => _autoSaveInterval;
        set { _autoSaveInterval = value; OnPropertyChanged(); }
    }

    private bool _showGrid = true;
    public bool ShowGrid
    {
        get => _showGrid;
        set { _showGrid = value; OnPropertyChanged(); }
    }

    private bool _showGizmos = true;
    public bool ShowGizmos
    {
        get => _showGizmos;
        set { _showGizmos = value; OnPropertyChanged(); }
    }

    private bool _showStats = false;
    public bool ShowStats
    {
        get => _showStats;
        set { _showStats = value; OnPropertyChanged(); }
    }

    // Language
    private int _selectedLanguageIndex;
    public int SelectedLanguageIndex
    {
        get => _selectedLanguageIndex;
        set { _selectedLanguageIndex = value; OnPropertyChanged(); }
    }

    public string[] Languages { get; } = { "English", "中文 (Chinese)" };

    public string WorkspaceText => string.Format(
        LocalizationService.Instance.T("settings.workspace"),
        ResolveWorkspace());

    private static string ResolveWorkspace()
    {
        var dir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")) ||
                Directory.Exists(Path.Combine(dir.FullName, "core")))
                return dir.FullName;
            dir = dir.Parent;
        }
        return AppDomain.CurrentDomain.BaseDirectory;
    }

    // Commands
    public ICommand ApplyCommand { get; }
    public ICommand CancelCommand { get; }
    public ICommand ResetCommand { get; }

    public SettingsViewModel()
    {
        _selectedLanguageIndex = (int)LocalizationService.Instance.Language;
        ApplyCommand = new RelayCommand(Apply);
        CancelCommand = new RelayCommand(Cancel);
        ResetCommand = new RelayCommand(ResetToDefaults);
    }

    private void Apply()
    {
        // Apply settings to the engine
        ApplyRequested?.Invoke();
    }

    private void Cancel()
    {
        CancelRequested?.Invoke();
    }

    private void ResetToDefaults()
    {
        IsDarkTheme = true;
        VSyncEnabled = true;
        AutoSaveInterval = 300;
        ShowGrid = true;
        ShowGizmos = true;
        ShowStats = false;
        SelectedLanguageIndex = 0;
    }

    public event Action? ApplyRequested;
    public event Action? CancelRequested;

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
