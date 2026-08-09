using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace GryceEngine.Editor.Models;

public enum LogLevel { Info, Warning, Error }

public class LogEntry : INotifyPropertyChanged
{
    public DateTime Timestamp { get; }
    public LogLevel Level { get; }
    public string Message { get; }
    public string SourceFile { get; }
    public int SourceLine { get; }
    public string LevelText => Level switch
    {
        LogLevel.Warning => "Warning",
        LogLevel.Error => "Error",
        _ => "Info"
    };

    public LogEntry(LogLevel level, string message, string sourceFile = "", int sourceLine = 0)
    {
        Timestamp = DateTime.Now;
        Level = level;
        Message = message;
        SourceFile = sourceFile;
        SourceLine = sourceLine;
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
