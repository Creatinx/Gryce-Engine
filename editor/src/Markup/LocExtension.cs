using GryceEngine.Editor.Services;
using System;
using System.Windows.Data;
using System.Windows.Markup;

namespace GryceEngine.Editor.Markup;

/// <summary>
/// Markup extension for localized strings: {loc:Loc menu.file}
/// Returns a binding against LocalizationService.Instance so the UI updates
/// immediately when the language is switched at runtime.
/// </summary>
public class LocExtension : MarkupExtension
{
    public string Key { get; set; } = string.Empty;

    public LocExtension() { }

    public LocExtension(string key)
    {
        Key = key;
    }

    public override object ProvideValue(IServiceProvider serviceProvider)
    {
        var binding = new Binding($"[{Key}]")
        {
            Source = LocalizationService.Instance,
            Mode = BindingMode.OneWay
        };
        return binding.ProvideValue(serviceProvider);
    }
}
