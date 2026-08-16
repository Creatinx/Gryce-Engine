using GryceEngine.Editor.Models;
using GryceEngine.Editor.ViewModels;
using System;
using System.Collections.Specialized;
using System.Linq;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;

namespace GryceEngine.Editor.Views;

public partial class ConsoleView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;
    private bool _showInfo = true;
    private bool _showWarning = true;
    private bool _showError = true;
    private LogEntry? _selectedEntry;
    private EditorViewModel? _subscribedVm;

    public ConsoleView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) =>
        {
            if (_subscribedVm != null)
                _subscribedVm.LogEntries.CollectionChanged -= OnLogEntriesChanged;
            _subscribedVm = VM;
            if (_subscribedVm != null)
                _subscribedVm.LogEntries.CollectionChanged += OnLogEntriesChanged;
        };
    }

    private void OnLogEntriesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        UpdateEntryCount();
        AutoScrollToBottom();
    }

    private void OnClearClick(object sender, RoutedEventArgs e)
    {
        VM?.ClearConsole();
        UpdateEntryCount();
        HideDetailPanel();
    }

    private void OnCopyClick(object sender, RoutedEventArgs e)
    {
        if (_selectedEntry != null)
        {
            Clipboard.SetText($"[{_selectedEntry.Timestamp:HH:mm:ss}] [{_selectedEntry.LevelText}] {_selectedEntry.Message}");
        }
    }

    /// <summary>Double-click a log entry to open its source location in VSCode
    /// (falls back to the OS default editor).</summary>
    private void OnLogDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (_selectedEntry == null) return;

        // Lua 脚本错误优先：消息形如 "scripts/player.lua:12: ..."，
        // 解析出脚本路径与行号跳转（相对路径基于项目根解析）。
        string file = _selectedEntry.SourceFile;
        int line = _selectedEntry.SourceLine;
        var luaMatch = Regex.Match(_selectedEntry.Message, @"([^\s""']+\.lua):(\d+)");
        if (luaMatch.Success)
        {
            string luaPath = luaMatch.Groups[1].Value;
            if (!Path.IsPathRooted(luaPath))
            {
                string? root = App.Engine?.ProjectRoot;
                if (!string.IsNullOrEmpty(root))
                    luaPath = Path.Combine(root, luaPath);
            }
            if (File.Exists(luaPath))
            {
                file = luaPath;
                line = int.Parse(luaMatch.Groups[2].Value);
            }
        }
        if (string.IsNullOrEmpty(file)) return;
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo("code")
            {
                Arguments = $"--goto \"{file}:{line}\"",
                UseShellExecute = false
            };
            System.Diagnostics.Process.Start(psi);
        }
        catch
        {
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(file)
                {
                    UseShellExecute = true
                });
            }
            catch { /* no default handler */ }
        }
    }

    private void OnCopyDetailClick(object sender, RoutedEventArgs e)
    {
        if (_selectedEntry != null)
        {
            Clipboard.SetText(_selectedEntry.Message);
        }
    }

    private void OnLogSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LogListBox.SelectedItem is LogEntry entry)
        {
            _selectedEntry = entry;
            ShowDetailPanel(entry);
        }
        else
        {
            _selectedEntry = null;
            HideDetailPanel();
        }
    }

    private void ShowDetailPanel(LogEntry entry)
    {
        DetailText.Text = entry.Message;
        DetailPanel.Visibility = Visibility.Visible;
    }

    private void HideDetailPanel()
    {
        DetailPanel.Visibility = Visibility.Collapsed;
        _selectedEntry = null;
    }

    private void OnFilterChanged(object sender, RoutedEventArgs e)
    {
        _showInfo = FilterInfo.IsChecked == true;
        _showWarning = FilterWarning.IsChecked == true;
        _showError = FilterError.IsChecked == true;
        ApplyFilter();
    }

    private void ApplyFilter()
    {
        if (VM == null) return;
        var collectionView = CollectionViewSource.GetDefaultView(VM.LogEntries);
        collectionView.Filter = item =>
        {
            if (item is LogEntry entry)
            {
                return entry.Level switch
                {
                    LogLevel.Warning => _showWarning,
                    LogLevel.Error => _showError,
                    _ => _showInfo
                };
            }
            return true;
        };
    }

    private void UpdateEntryCount()
    {
        if (VM == null) return;

        int total = VM.LogEntries.Count;
        int infoCount = VM.LogEntries.Count(e => e.Level == LogLevel.Info);
        int warnCount = VM.LogEntries.Count(e => e.Level == LogLevel.Warning);
        int errCount = VM.LogEntries.Count(e => e.Level == LogLevel.Error);

        EntryCount.Text = $"{total} entries";

        // Update filter button labels with counts
        InfoCountLabel.Text = infoCount > 0 ? $"Info ({infoCount})" : "Info";
        WarningCountLabel.Text = warnCount > 0 ? $"Warning ({warnCount})" : "Warning";
        ErrorCountLabel.Text = errCount > 0 ? $"Error ({errCount})" : "Error";
    }

    private void AutoScrollToBottom()
    {
        if (VM == null || VM.LogEntries.Count == 0) return;

        // Defer to allow the ListBox to render the new item
        Dispatcher.BeginInvoke(new Action(() =>
        {
            if (LogListBox.Items.Count > 0)
            {
                var lastItem = LogListBox.Items[LogListBox.Items.Count - 1];
                LogListBox.ScrollIntoView(lastItem);
            }
        }), System.Windows.Threading.DispatcherPriority.Background);
    }
}
