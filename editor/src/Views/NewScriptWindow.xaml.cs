using GryceEngine.Editor.Services;
using GryceEngine.Editor.Native;
using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;

namespace GryceEngine.Editor.Views;

/// <summary>
/// "New Script" dialog: name, parent component, template (default node) and
/// target folder. Creates a .lua file with the selected template.
/// </summary>
public partial class NewScriptWindow : Window
{
    private sealed class TemplateItem
    {
        public TemplateItem(string key, string display) { Key = key; Display = display; }
        public string Key { get; }
        public string Display { get; }
        public override string ToString() => Display;
    }

    /// <summary>Full path of the created .lua file (valid after DialogResult == true).</summary>
    public string? CreatedPath { get; private set; }

    /// <summary>The selected base class type name (may be empty).</summary>
    public string? BaseClass => BaseClassBox.SelectedItem as string;

    /// <summary>The selected parent component (present in the scene).</summary>
    public string? ParentComponent => ParentComponentBox.SelectedItem as string;

    public NewScriptWindow(string initialFolder,
                           IEnumerable<string>? baseClasses = null,
                           string? initialBase = null,
                           IEnumerable<string>? sceneComponents = null,
                           string? initialParent = null)
    {
        InitializeComponent();

        // Template options (node is the default)
        var loc = LocalizationService.Instance;
        TemplateBox.ItemsSource = new[]
            {
                "node", "camera", "light", "physics"
            }
            .Select(k => new TemplateItem(k, loc.T("new_script.template." + k)))
            .ToList();
        TemplateBox.DisplayMemberPath = nameof(TemplateItem.Display);
        TemplateBox.SelectedIndex = 0;

        // Base class list (registered component types), optional
        var bases = baseClasses?.Where(p => !string.IsNullOrWhiteSpace(p)).ToList();
        if (bases != null && bases.Count > 0)
        {
            BaseClassBox.ItemsSource = bases;
            // Default base class: Node3D in 3D scenes, Node2D in 2D scenes
            // (falls back to the first registered type if neither is present).
            string preferred = SceneAPI.GScene_GetMode() == 0 ? "Node2D" : "Node3D";
            string defaultBase = !string.IsNullOrEmpty(initialBase) && bases.Contains(initialBase!)
                ? initialBase!
                : bases.Contains(preferred)
                    ? preferred
                    : bases.Contains("Node3D")
                        ? "Node3D"
                        : bases.Contains("Node2D")
                            ? "Node2D"
                            : bases[0];
            BaseClassBox.SelectedItem = defaultBase;
        }

        // Parent component list (components present in the scene), optional
        var parents = sceneComponents?.Where(p => !string.IsNullOrWhiteSpace(p)).ToList();
        if (parents != null && parents.Count > 0)
        {
            ParentComponentBox.ItemsSource = parents;
            ParentComponentBox.SelectedItem =
                !string.IsNullOrEmpty(initialParent) && parents.Contains(initialParent!)
                    ? initialParent
                    : parents[0];
        }

        PathBox.Text = NormalizeFolder(initialFolder);
        NameBox.Focus();
    }

    private static string NormalizeFolder(string folder)
    {
        if (string.IsNullOrWhiteSpace(folder)) return string.Empty;
        try
        {
            return Path.GetFullPath(folder);
        }
        catch
        {
            return folder;
        }
    }

    private void OnBrowseClick(object sender, RoutedEventArgs e)
    {
        var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            SelectedPath = Directory.Exists(PathBox.Text) ? PathBox.Text : string.Empty,
            Description = LocalizationService.Instance.T("new_script.path")
        };
        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            PathBox.Text = dialog.SelectedPath;
        }
    }

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }

    private void OnCreateClick(object sender, RoutedEventArgs e)
    {
        var loc = LocalizationService.Instance;
        string name = NameBox.Text?.Trim() ?? string.Empty;
        string folder = PathBox.Text?.Trim() ?? string.Empty;

        if (string.IsNullOrEmpty(name))
        {
            MessageBox.Show(this, loc.T("new_script.name_required"), loc.T("new_script.title"),
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (string.IsNullOrEmpty(folder))
        {
            MessageBox.Show(this, loc.T("new_script.path_required"), loc.T("new_script.title"),
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (!Directory.Exists(folder))
        {
            MessageBox.Show(this, loc.T("new_script.path_not_dir"), loc.T("new_script.title"),
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        // The Name field holds the base name (no .lua suffix); strip the
        // extension if the user typed it, then append ".lua" on creation.
        if (name.EndsWith(".lua", StringComparison.OrdinalIgnoreCase))
        {
            name = name.Substring(0, name.Length - 4);
        }
        string safeName = name.Trim();
        foreach (char c in Path.GetInvalidFileNameChars())
        {
            safeName = safeName.Replace(c, '_');
        }
        safeName += ".lua";

        string path = Path.Combine(folder, safeName);
        if (File.Exists(path))
        {
            MessageBox.Show(this, loc.T("new_script.already_exists"), loc.T("new_script.title"),
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        try
        {
            string template = (TemplateBox.SelectedItem as TemplateItem)?.Key ?? "node";
            string parent = ParentComponentBox.SelectedItem as string
                            ?? BaseClassBox.SelectedItem as string ?? string.Empty;
            File.WriteAllText(path, BuildTemplate(template, parent));
            CreatedPath = path;
            DialogResult = true;
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, string.Format(loc.T("new_script.failed"), ex.Message),
                loc.T("new_script.title"), MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private static string BuildTemplate(string template, string parentComponent)
    {
        string parentHint = string.IsNullOrEmpty(parentComponent)
            ? "generic entity"
            : parentComponent;

        string body = template switch
        {
            "camera" =>
                "props = {\r\n" +
                "    sensitivity = 1.0\r\n" +
                "}\r\n",
            "light" =>
                "props = {\r\n" +
                "    intensity = 1.0\r\n" +
                "}\r\n",
            "physics" =>
                "props = {\r\n" +
                "    force = 10.0\r\n" +
                "}\r\n",
            _ =>
                "props = {\r\n" +
                "    speed = 1.0\r\n" +
                "}\r\n"
        };

        return
            "-- GryceSRT script template\r\n" +
            "-- parent component: " + parentHint + "\r\n" +
            "require(\"GryceEngineUtils\")\r\n" +
            "\r\n" +
            body +
            "\r\n" +
            "function on_start()\r\n" +
            "    engine.log.info(\"on_start\")\r\n" +
            "end\r\n" +
            "\r\n" +
            "function on_update(dt)\r\n" +
            "    -- engine.self() / engine.entity.* / engine.component.* / GryceEngineUtils.*\r\n" +
            "end\r\n";
    }
}
