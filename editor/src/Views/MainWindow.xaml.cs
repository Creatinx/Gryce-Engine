using GryceEngine.Editor.Native;
using GryceEngine.Editor.ViewModels;
using iNKORE.UI.WPF.Modern;
using iNKORE.UI.WPF.Modern.Controls;
using Microsoft.Win32;
using System;
using System.Windows;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class MainWindow
{
    private EditorViewModel? VM => DataContext as EditorViewModel;

    public MainWindow(EditorViewModel viewModel)
    {
        DataContext = viewModel;
        InitializeComponent();
        viewModel.RefreshHierarchy();
    }

    // === File Menu ===

    private void OnLoadSceneClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = "Load Scene",
            Filter = "Gryce Scene Files (*.gesc)|*.gesc|All Files (*.*)|*.*",
            DefaultExt = ".gesc"
        };
        if (dialog.ShowDialog() == true)
        {
            int result = SceneAPI.GScene_Load(dialog.FileName);
            if (result == 0)
            {
                VM?.AppendConsole($"Scene loaded: {dialog.FileName}");
                VM?.SetSceneName(System.IO.Path.GetFileNameWithoutExtension(dialog.FileName));
            }
            else
                VM?.AppendConsole($"Failed to load scene: {dialog.FileName}");
        }
    }

    private void OnImportAssetClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = "Import Asset",
            Filter = "3D Models (*.obj;*.fbx;*.gltf;*.glb)|*.obj;*.fbx;*.gltf;*.glb|Textures (*.png;*.jpg;*.jpeg;*.dds)|*.png;*.jpg;*.jpeg;*.dds|All Files (*.*)|*.*",
            Multiselect = true
        };
        if (dialog.ShowDialog() == true)
        {
            foreach (var file in dialog.FileNames)
            {
                var handle = AssetAPI.GAsset_Import(file);
                if (handle != GAssetHandle.Null)
                    VM?.AppendConsole($"Imported: {file} -> handle {handle}");
                else
                    VM?.AppendConsole($"Failed to import: {file}");
            }
        }
    }

    private void OnExitClick(object sender, RoutedEventArgs e)
    {
        Application.Current.Shutdown();
    }

    private void OnCreateEntityClick(object sender, RoutedEventArgs e)
    {
        var dialog = new CreateEntityDialog(VM!, GEntityHandle.Null)
        {
            Owner = this
        };
        dialog.ShowDialog();
    }

    // === View Menu ===

    private void OnRefreshViewClick(object sender, RoutedEventArgs e)
    {
        VM?.RefreshHierarchy();
        VM?.AppendConsole("View refreshed.");
    }

    // === Theme Switching ===

    private void OnThemeDarkClick(object sender, RoutedEventArgs e)
    {
        ApplyTheme(ElementTheme.Dark);
        if (VM != null) VM.CurrentTheme = "Dark";
    }

    private void OnThemeLightClick(object sender, RoutedEventArgs e)
    {
        ApplyTheme(ElementTheme.Light);
        if (VM != null) VM.CurrentTheme = "Light";
    }

    private static void ApplyTheme(ElementTheme theme)
    {
        if (Application.Current.MainWindow is Window mainWindow)
        {
            ThemeManager.SetRequestedTheme(mainWindow, theme);
        }
    }

    // === Settings & About ===

    private void OnSettingsClick(object sender, RoutedEventArgs e)
    {
        var settingsWindow = new SettingsWindow { Owner = this };
        if (settingsWindow.ShowDialog() == true)
        {
            VM?.AppendConsole("Settings applied.");
        }
    }

    private void OnAboutClick(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            Title = "About Gryce Engine",
            Content = "Gryce Engine Editor\n\nA modern game engine built with C++ and WPF.\nFluent Design | Unity-style Layout\n\nVersion 0.1.0",
            CloseButtonText = "OK",
            DefaultButton = ContentDialogButton.Close
        };
        _ = dialog.ShowAsync();
    }
}
