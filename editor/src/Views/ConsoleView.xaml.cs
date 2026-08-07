using GryceEngine.Editor.Models;
using GryceEngine.Editor.ViewModels;
using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;

namespace GryceEngine.Editor.Views;

public partial class ConsoleView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;
    private bool _showInfo = true;
    private bool _showWarning = true;
    private bool _showError = true;
    private LogEntry? _selectedEntry;

    public ConsoleView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) =>
        {
            if (VM != null)
                VM.LogEntries.CollectionChanged += (_, _) =>
                {
                    UpdateEntryCount();
                    AutoScrollToBottom();
                };
        };
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