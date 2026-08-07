using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Globalization;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace GryceEngine.Editor.Views;

/// <summary>
/// PBR material editor for the selected MeshRenderer / SkinnedMeshRenderer.
/// All changes are written to the engine immediately through the C API.
/// </summary>
public partial class MaterialEditorWindow : Window
{
    private readonly GEntityHandle _entity;
    private readonly ulong _compHash;
    private bool _syncing;

    private static readonly (GMaterialField Path, GMaterialField Use, string Key)[] MapSlots =
    {
        (GMaterialField.AlbedoMapPath, GMaterialField.UseAlbedoMap, "material_editor.albedo_map"),
        (GMaterialField.NormalMapPath, GMaterialField.UseNormalMap, "material_editor.normal_map"),
        (GMaterialField.RoughnessMapPath, GMaterialField.UseRoughnessMap, "material_editor.roughness_map"),
        (GMaterialField.MetallicMapPath, GMaterialField.UseMetallicMap, "material_editor.metallic_map"),
        (GMaterialField.AoMapPath, GMaterialField.UseAoMap, "material_editor.ao_map"),
        (GMaterialField.EmissiveMapPath, GMaterialField.UseEmissiveMap, "material_editor.emissive_map")
    };

    public MaterialEditorWindow(EditorViewModel vm, EntityModel entity, ComponentModel renderer)
    {
        InitializeComponent();
        _entity = entity.Handle;
        _compHash = renderer.TypeHash;
        TitleText.Text = $"{LocalizationService.Instance.T("material_editor.title")} — {entity.Name}";

        BlendCombo.Items.Add("Opaque");
        BlendCombo.Items.Add("Blend");

        RoughnessBox.Tag = GMaterialField.Roughness;
        MetallicBox.Tag = GMaterialField.Metallic;
        OpacityBox.Tag = GMaterialField.Opacity;

        BuildMapFields();
        LoadAll();
    }

    private void BuildMapFields()
    {
        foreach (var (path, use, key) in MapSlots)
        {
            var row = new Grid { Margin = new Thickness(0, 4, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new System.Windows.GridLength(120) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new System.Windows.GridLength(1, System.Windows.GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = System.Windows.GridLength.Auto });

            var label = new TextBlock
            {
                Text = LocalizationService.Instance.T(key),
                Style = (Style)FindResource("PropertyLabelStyle"),
                VerticalAlignment = VerticalAlignment.Center
            };
            var box = new TextBox
            {
                FontSize = 10.5,
                Padding = new Thickness(5, 2, 5, 2),
                Margin = new Thickness(6, 0, 6, 0),
                VerticalContentAlignment = VerticalAlignment.Center
            };
            box.Tag = path;
            box.LostFocus += OnPathChanged;

            var useCheck = new CheckBox
            {
                Content = LocalizationService.Instance.T("material_editor.use_map"),
                FontSize = 10.5,
                VerticalAlignment = VerticalAlignment.Center,
                Tag = use
            };
            useCheck.Checked += OnFlagChanged;
            useCheck.Unchecked += OnFlagChanged;

            Grid.SetColumn(label, 0);
            Grid.SetColumn(box, 1);
            Grid.SetColumn(useCheck, 2);
            row.Children.Add(label);
            row.Children.Add(box);
            row.Children.Add(useCheck);
            MapFields.Children.Add(row);
        }
    }

    private void LoadAll()
    {
        _syncing = true;
        try
        {
            SetText(AlbedoR, GetVec3(GMaterialField.AlbedoColor, 0));
            SetText(AlbedoG, GetVec3(GMaterialField.AlbedoColor, 1));
            SetText(AlbedoB, GetVec3(GMaterialField.AlbedoColor, 2));
            SetText(RoughnessBox, GetFloat(GMaterialField.Roughness));
            SetText(MetallicBox, GetFloat(GMaterialField.Metallic));
            SetText(OpacityBox, GetFloat(GMaterialField.Opacity));

            TwoSidedCheck.IsChecked = GetFloat(GMaterialField.TwoSided) > 0.5f;
            BlendCombo.SelectedIndex = GetFloat(GMaterialField.BlendMode) > 0.5f ? 1 : 0;

            int i = 0;
            foreach (var (path, use, _) in MapSlots)
            {
                if (i < MapFields.Children.Count && MapFields.Children[i] is Grid row)
                {
                    if (row.Children[1] is TextBox box) box.Text = GetString(path);
                    if (row.Children[2] is CheckBox chk) chk.IsChecked = GetFloat(use) > 0.5f;
                }
                i++;
            }
        }
        finally
        {
            _syncing = false;
        }
    }

    private static void SetText(TextBox box, float value) =>
        box.Text = value.ToString("0.###", CultureInfo.InvariantCulture);

    private static float ReadFloat(TextBox box, float fallback) =>
        float.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;

    private float GetFloat(GMaterialField field)
    {
        var floats = new float[3];
        if (MaterialAPI.GMaterial_GetField(_entity, _compHash, (int)field,
                floats, floats.Length, null!, 0) == 0)
            return floats[0];
        return 0;
    }

    private float GetVec3(GMaterialField field, int component)
    {
        var floats = new float[3];
        if (MaterialAPI.GMaterial_GetField(_entity, _compHash, (int)field,
                floats, floats.Length, null!, 0) == 0)
            return floats[component];
        return 0;
    }

    private string GetString(GMaterialField field)
    {
        var sb = new StringBuilder(512);
        if (MaterialAPI.GMaterial_GetField(_entity, _compHash, (int)field,
                null!, 0, sb, sb.Capacity) >= 0)
            return sb.ToString();
        return string.Empty;
    }

    private void SetFloat(GMaterialField field, float value) =>
        MaterialAPI.GMaterial_SetField(_entity, _compHash, (int)field, new[] { value }, 1, null);

    private void SetVec3(GMaterialField field, float x, float y, float z) =>
        MaterialAPI.GMaterial_SetField(_entity, _compHash, (int)field, new[] { x, y, z }, 3, null);

    private void SetString(GMaterialField field, string value) =>
        MaterialAPI.GMaterial_SetField(_entity, _compHash, (int)field, null!, 0, value);

    // === 即时写回引擎 ===

    private void OnAlbedoChanged(object sender, TextChangedEventArgs e)
    {
        if (_syncing) return;
        // TextChanged 可能在 XAML 解析阶段（InitializeComponent 完成前）触发，
        // 此时控件字段尚未赋值。
        if (AlbedoR == null || AlbedoG == null || AlbedoB == null) return;
        SetVec3(GMaterialField.AlbedoColor,
            ReadFloat(AlbedoR, 1), ReadFloat(AlbedoG, 1), ReadFloat(AlbedoB, 1));
        // 没有反照率贴图时关闭 use 标志，让颜色直接生效（否则采样白色兜底贴图）。
        if (GetMapPathBox(0) is { } albedoBox && string.IsNullOrWhiteSpace(albedoBox.Text))
            SetFloat(GMaterialField.UseAlbedoMap, 0);
    }

    private void OnFloatChanged(object sender, TextChangedEventArgs e)
    {
        if (_syncing || sender is not TextBox box || box.Tag is not GMaterialField field) return;
        SetFloat(field, ReadFloat(box, 0));
    }

    private void OnFlagChanged(object sender, RoutedEventArgs e)
    {
        if (_syncing) return;
        if (sender is CheckBox chk && chk.Tag is GMaterialField field)
            SetFloat(field, chk.IsChecked == true ? 1 : 0);
        else if (sender is CheckBox twoSided && twoSided == TwoSidedCheck)
            SetFloat(GMaterialField.TwoSided, twoSided.IsChecked == true ? 1 : 0);
    }

    private void OnBlendChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing) return;
        SetFloat(GMaterialField.BlendMode, BlendCombo.SelectedIndex == 1 ? 1 : 0);
    }

    private void OnPathChanged(object sender, RoutedEventArgs e)
    {
        if (_syncing || sender is not TextBox box || box.Tag is not GMaterialField field) return;
        string path = box.Text.Trim();
        SetString(field, path);
        // 填了路径就启用对应 use 标志，让贴图生效。
        if (!string.IsNullOrWhiteSpace(path))
        {
            foreach (var (pathField, useField, _) in MapSlots)
            {
                if (pathField == field)
                {
                    SetFloat(useField, 1);
                    break;
                }
            }
        }
    }

    private TextBox? GetMapPathBox(int index)
    {
        // InitializeComponent 解析阶段 MapFields 尚未赋值
        if (MapFields == null) return null;
        if (index < 0 || index >= MapFields.Children.Count) return null;
        return MapFields.Children[index] is Grid row && row.Children.Count > 1
            ? row.Children[1] as TextBox
            : null;
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => Close();
}
