using System.Windows;

namespace GryceEngine.Editor.Views;

/// <summary>Theme-aware confirmation dialog (used e.g. for file deletion).</summary>
public partial class ConfirmDialog : Window
{
    public bool Confirmed { get; private set; }

    public ConfirmDialog(string message, string? title = null)
    {
        InitializeComponent();
        MessageText.Text = message;
        if (!string.IsNullOrEmpty(title))
        {
            Title = title;
        }
    }

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        Confirmed = false;
        DialogResult = false;
    }

    private void OnConfirmClick(object sender, RoutedEventArgs e)
    {
        Confirmed = true;
        DialogResult = true;
    }
}
