using System.Windows;
using System.Windows.Controls;

namespace GryceEngine.Editor.Views;

/// <summary>Simple input dialog for renaming entities and creating folders.</summary>
public class InputDialog : Window
{
    public string InputText { get; private set; } = string.Empty;

    public InputDialog(string title, string prompt, string defaultText = "")
    {
        Title = title;
        Width = 320;
        Height = 150;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        ResizeMode = ResizeMode.NoResize;
        WindowStyle = WindowStyle.ToolWindow;

        var grid = new Grid { Margin = new Thickness(12) };
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        var promptLabel = new TextBlock { Text = prompt, Margin = new Thickness(0, 0, 0, 8) };
        Grid.SetRow(promptLabel, 0);

        var textBox = new TextBox { Text = defaultText, Margin = new Thickness(0, 0, 0, 12) };
        textBox.SelectAll();
        textBox.Focus();
        Grid.SetRow(textBox, 1);

        var buttonPanel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right
        };
        var okButton = new Button { Content = "OK", Width = 72, Height = 26, IsDefault = true };
        var cancelButton = new Button { Content = "Cancel", Width = 72, Height = 26, IsCancel = true, Margin = new Thickness(8, 0, 0, 0) };

        okButton.Click += (_, _) =>
        {
            InputText = textBox.Text;
            DialogResult = true;
            Close();
        };
        cancelButton.Click += (_, _) =>
        {
            DialogResult = false;
            Close();
        };

        buttonPanel.Children.Add(okButton);
        buttonPanel.Children.Add(cancelButton);
        Grid.SetRow(buttonPanel, 2);

        grid.Children.Add(promptLabel);
        grid.Children.Add(textBox);
        grid.Children.Add(buttonPanel);
        Content = grid;

        Loaded += (_, _) => textBox.Focus();
    }
}