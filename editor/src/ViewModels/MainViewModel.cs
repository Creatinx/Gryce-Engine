using GryceEngine.Editor.Services;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace GryceEngine.Editor.ViewModels;

public class MainViewModel : INotifyPropertyChanged
{
    private readonly EngineService _engine;

    private string _title = "Gryce Engine Editor";
    public string Title { get => _title; set { _title = value; OnPropertyChanged(); } }

    private bool _isPlaying;
    public bool IsPlaying { get => _isPlaying; set { _isPlaying = value; OnPropertyChanged(); } }

    private bool _isPaused;
    public bool IsPaused { get => _isPaused; set { _isPaused = value; OnPropertyChanged(); } }

    public ICommand PlayCommand { get; }
    public ICommand PauseCommand { get; }
    public ICommand StopCommand { get; }

    public MainViewModel(EngineService engine)
    {
        _engine = engine;
        _engine.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(EngineService.IsPlaying)) IsPlaying = _engine.IsPlaying;
            if (e.PropertyName == nameof(EngineService.IsPaused)) IsPaused = _engine.IsPaused;
        };
        PlayCommand = new RelayCommand(_engine.Play);
        PauseCommand = new RelayCommand(_engine.Pause);
        StopCommand = new RelayCommand(_engine.Stop);
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

public class RelayCommand : ICommand
{
    private readonly Action _execute;
    private readonly Func<bool>? _canExecute;

    public RelayCommand(Action execute, Func<bool>? canExecute = null)
    {
        _execute = execute;
        _canExecute = canExecute;
    }

    public event EventHandler? CanExecuteChanged
    {
        add => CommandManager.RequerySuggested += value;
        remove => CommandManager.RequerySuggested -= value;
    }

    public bool CanExecute(object? parameter) => _canExecute?.Invoke() ?? true;
    public void Execute(object? parameter) => _execute();
}
