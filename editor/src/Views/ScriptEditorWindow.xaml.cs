using GryceEngine.Editor.ViewModels;
using System;
using System.Collections.Generic;
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

    private static readonly string[] LuaKeywords =
    {
        "and","break","do","else","elseif","end","false","for","function","goto",
        "if","in","local","nil","not","or","repeat","return","then","true","until","while"
    };

    private static readonly Brush KeywordBrush = new SolidColorBrush(Color.FromRgb(0xC5, 0x86, 0xC0));
    private static readonly Brush StringBrush = new SolidColorBrush(Color.FromRgb(0x6A, 0x99, 0x55));
    private static readonly Brush CommentBrush = new SolidColorBrush(Color.FromRgb(0x57, 0x7A, 0x7A));
    private static readonly Brush NumberBrush = new SolidColorBrush(Color.FromRgb(0xB5, 0xCE, 0xA8));
    private static readonly Brush DefaultBrush = new SolidColorBrush(Color.FromRgb(0xD4, 0xD4, 0xD4));

    public ScriptEditorWindow(EditorViewModel vm, string path)
    {
        InitializeComponent();
        _vm = vm;
        _path = path;
        Title = $"Lua Script - {Path.GetFileName(path)}";
        PathText.Text = path;

        LoadFile();

        // Undo/Redo: RichTextBox already binds Ctrl+Z / Ctrl+Y, but make the
        // commands explicit at window level so they never get swallowed.
        CommandBindings.Add(new CommandBinding(ApplicationCommands.Undo,
            (_, _) => EditorBox.Undo(),
            (_, e) => e.CanExecute = CanUndo()));
        CommandBindings.Add(new CommandBinding(ApplicationCommands.Redo,
            (_, _) => EditorBox.Redo(),
            (_, e) => e.CanExecute = CanRedo()));

        EditorBox.LostFocus += (_, _) => ApplyHighlighting();
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
        // Do NOT rebuild highlighting while typing: replacing the Document
        // clears the RichTextBox undo stack, so Ctrl+Z/Y would stop working.
        // Highlighting is reapplied on focus loss instead.
        UpdateUndoButtons();
    }

    private void OnEditorPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.S && Keyboard.Modifiers == ModifierKeys.Control)
        {
            e.Handled = true;
            SaveFile();
        }
    }

    private void OnUndoClick(object sender, RoutedEventArgs e) => EditorBox.Undo();
    private void OnRedoClick(object sender, RoutedEventArgs e) => EditorBox.Redo();

    private bool CanUndo() => EditorBox.CanUndo;
    private bool CanRedo() => EditorBox.CanRedo;

    private void UpdateUndoButtons()
    {
        if (BtnUndo != null) BtnUndo.IsEnabled = EditorBox.CanUndo;
        if (BtnRedo != null) BtnRedo.IsEnabled = EditorBox.CanRedo;
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
        if (_updating || EditorBox.Document == null ||
            EditorBox.Document.Blocks.FirstBlock is not Paragraph) return;
        _updating = true;
        try
        {
            var text = new TextRange(EditorBox.Document.ContentStart,
                                     EditorBox.Document.ContentEnd).Text;
            // Color in place without rebuilding the FlowDocument, so the
            // RichTextBox undo stack survives highlighting.
            new TextRange(EditorBox.Document.ContentStart, EditorBox.Document.ContentEnd)
                .ApplyPropertyValue(TextElement.ForegroundProperty, DefaultBrush);
            foreach (var (start, length, brush) in CollectTokens(text))
            {
                var p1 = EditorBox.Document.ContentStart.GetPositionAtOffset(start);
                var p2 = EditorBox.Document.ContentStart.GetPositionAtOffset(start + length);
                if (p1 != null && p2 != null)
                {
                    new TextRange(p1, p2)
                        .ApplyPropertyValue(TextElement.ForegroundProperty, brush);
                }
            }
            UpdateLineNumbers();
        }
        catch { }
        finally { _updating = false; }
    }

    private static List<(int Start, int Length, Brush Brush)> CollectTokens(string text)
    {
        var tokens = new List<(int, int, Brush)>();

        int i = 0;
        while (i < text.Length)
        {
            char c = text[i];

            // Line comment
            if (c == '-' && i + 1 < text.Length && text[i + 1] == '-')
            {
                int end = text.IndexOf('\n', i);
                if (end < 0) end = text.Length;
                tokens.Add((i, end - i, CommentBrush));
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
                tokens.Add((i, end - i, StringBrush));
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
                tokens.Add((i, end - i, keyword ? KeywordBrush : DefaultBrush));
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
                tokens.Add((i, end - i, NumberBrush));
                i = end;
                continue;
            }
            i++;
        }
        return tokens;
    }
}
