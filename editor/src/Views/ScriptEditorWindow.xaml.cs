using GryceEngine.Editor.ViewModels;
using System;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Minimal .lua script editor (GryceSRT): line numbers, basic Lua syntax
/// highlighting, Ctrl+S save, and a Reload button that pushes
/// ECMD_RELOAD_SCRIPTS so changes apply on the next engine frame.
/// </summary>
public partial class ScriptEditorWindow : Window
{
    private readonly EditorViewModel _vm;
    private readonly string _path;
    private bool _dirty;
    private bool _updating;
    private System.Windows.Threading.DispatcherTimer? _highlightTimer;

    private static readonly string[] LuaKeywords =
    {
        "and","break","do","else","elseif","end","false","for","function","goto",
        "if","in","local","nil","not","or","repeat","return","then","true","until","while"
    };

    public ScriptEditorWindow(EditorViewModel vm, string path)
    {
        InitializeComponent();
        _vm = vm;
        _path = path;
        Title = $"Lua Script - {Path.GetFileName(path)}";
        PathText.Text = path;

        LoadFile();

        // Debounced highlighting so typing stays responsive.
        _highlightTimer = new System.Windows.Threading.DispatcherTimer(
            System.Windows.Threading.DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(180)
        };
        _highlightTimer.Tick += (_, _) =>
        {
            _highlightTimer.Stop();
            ApplyHighlighting();
        };
    }

    private void LoadFile()
    {
        try
        {
            EditorBox.Document.Blocks.Clear();
            EditorBox.AppendText(File.ReadAllText(_path));
            ApplyHighlighting();
            StatusText.Text = "Loaded";
        }
        catch (Exception ex)
        {
            StatusText.Text = ex.Message;
        }
    }

    private void OnEditorTextChanged(object sender, TextChangedEventArgs e)
    {
        if (_updating) return;
        _dirty = true;
        StatusText.Text = "Modified";
        UpdateLineNumbers();
        _highlightTimer?.Stop();
        _highlightTimer?.Start();
    }

    private void OnEditorPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.S && Keyboard.Modifiers == ModifierKeys.Control)
        {
            e.Handled = true;
            SaveFile();
        }
    }

    private void OnSaveClick(object sender, RoutedEventArgs e) => SaveFile();

    private void SaveFile()
    {
        try
        {
            string text = new TextRange(EditorBox.Document.ContentStart,
                                        EditorBox.Document.ContentEnd).Text;
            File.WriteAllText(_path, text);
            _dirty = false;
            StatusText.Text = "Saved";
            _vm.ReloadScripts();
        }
        catch (Exception ex)
        {
            StatusText.Text = ex.Message;
        }
    }

    private void OnReloadClick(object sender, RoutedEventArgs e)
    {
        _vm.ReloadScripts();
        StatusText.Text = "Reload requested";
    }

    // --- line numbers ------------------------------------------------------
    private void UpdateLineNumbers()
    {
        int lines = 1;
        foreach (var para in EditorBox.Document.Blocks)
        {
            if (para is Paragraph p)
            {
                var text = new TextRange(p.ContentStart, p.ContentEnd).Text;
                lines += CountLines(text);
            }
        }
        var sb = new System.Text.StringBuilder();
        for (int i = 1; i <= lines; i++)
        {
            if (i > 1) sb.Append('\n');
            sb.Append(i);
        }
        LineNumbers.Text = sb.ToString();
    }

    private static int CountLines(string text)
    {
        int n = 0;
        foreach (char c in text)
        {
            if (c == '\n') n++;
        }
        return n;
    }

    // --- Lua syntax highlighting -------------------------------------------
    private void ApplyHighlighting()
    {
        if (_updating || EditorBox.Document == null) return;
        _updating = true;

        var caretOffset = new TextRange(EditorBox.Document.ContentStart,
                                        EditorBox.CaretPosition).Text.Length;
        var text = new TextRange(EditorBox.Document.ContentStart,
                                 EditorBox.Document.ContentEnd).Text;

        var doc = new FlowDocument();
        var para = new Paragraph { Margin = new Thickness(0) };
        doc.Blocks.Add(para);

        HighlightTokens(text, para);
        EditorBox.Document = doc;

        try
        {
            var end = EditorBox.Document.ContentStart.GetPositionAtOffset(
                Math.Min(caretOffset, text.Length));
            EditorBox.CaretPosition = end ?? EditorBox.Document.ContentEnd;
        }
        catch { }

        UpdateLineNumbers();
        _updating = false;
    }

    private void HighlightTokens(string text, Paragraph para)
    {
        var keywordBrush = new SolidColorBrush(Color.FromRgb(0xC5, 0x86, 0xC0));
        var stringBrush = new SolidColorBrush(Color.FromRgb(0x6A, 0x99, 0x55));
        var commentBrush = new SolidColorBrush(Color.FromRgb(0x57, 0x7A, 0x7A));
        var numberBrush = new SolidColorBrush(Color.FromRgb(0xB5, 0xCE, 0xA8));
        var defaultBrush = new SolidColorBrush(Color.FromRgb(0xD4, 0xD4, 0xD4));

        int i = 0;
        while (i < text.Length)
        {
            char c = text[i];

            // Line comment
            if (c == '-' && i + 1 < text.Length && text[i + 1] == '-')
            {
                int end = text.IndexOf('\n', i);
                if (end < 0) end = text.Length;
                AppendRun(para, text.Substring(i, end - i), commentBrush);
                i = end;
                continue;
            }

            // String
            if (c == '"' || c == '\'')
            {
                int end = i + 1;
                while (end < text.Length && text[end] != c)
                {
                    if (text[end] == '\\') end++;
                    end++;
                }
                if (end < text.Length) end++;
                AppendRun(para, text.Substring(i, end - i), stringBrush);
                i = end;
                continue;
            }

            // Word
            if (char.IsLetter(c) || c == '_')
            {
                int end = i;
                while (end < text.Length &&
                       (char.IsLetterOrDigit(text[end]) || text[end] == '_')) end++;
                string word = text.Substring(i, end - i);
                bool keyword = Array.IndexOf(LuaKeywords, word) >= 0;
                AppendRun(para, word, keyword ? keywordBrush : defaultBrush);
                i = end;
                continue;
            }

            // Number
            if (char.IsDigit(c) || (c == '.' && i + 1 < text.Length && char.IsDigit(text[i + 1])))
            {
                int end = i;
                while (end < text.Length &&
                       (char.IsDigit(text[end]) || text[end] == '.' ||
                        text[end] == 'e' || text[end] == 'E' ||
                        text[end] == 'x' || text[end] == 'X' ||
                        (text[end] == '-' || text[end] == '+') && end > i &&
                        (text[end - 1] == 'e' || text[end - 1] == 'E')))
                    end++;
                AppendRun(para, text.Substring(i, end - i), numberBrush);
                i = end;
                continue;
            }

            AppendRun(para, c.ToString(), defaultBrush);
            i++;
        }
    }

    private static void AppendRun(Paragraph para, string text, Brush brush)
    {
        if (text.Length == 0) return;
        var run = new Run(text) { Foreground = brush };
        para.Inlines.Add(run);
    }
}
