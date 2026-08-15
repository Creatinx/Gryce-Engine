using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using Microsoft.Win32;
using System;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Tilemap 数据编辑器：设置瓦片集/地图尺寸/格子尺寸，并在网格上按调色板
/// 绘制瓦片索引（左键画、右键擦除）。标量字段走反射属性 API，
/// tiles 向量走专用 GComponent_Tilemap* 数组 API。
/// </summary>
public partial class TilemapEditorWindow : Window
{
    private readonly GEntityHandle _entity;
    private readonly ulong _typeHash;
    private int[] _tiles = Array.Empty<int>();
    private int _width;
    private int _height;
    private int _selectedIndex;

    private static readonly Color[] PaletteColors =
    {
        Color.FromRgb(0xFF, 0x45, 0x45),
        Color.FromRgb(0x45, 0xFF, 0x45),
        Color.FromRgb(0x45, 0x8C, 0xFF),
        Color.FromRgb(0xFF, 0xD0, 0x45),
        Color.FromRgb(0x45, 0xFF, 0xFF),
        Color.FromRgb(0xFF, 0x45, 0xFF),
        Color.FromRgb(0xFF, 0xFF, 0xFF),
        Color.FromRgb(0xAA, 0xAA, 0xAA)
    };

    public TilemapEditorWindow(GEntityHandle entity, ulong typeHash, string tilesetPath)
    {
        InitializeComponent();
        _entity = entity;
        _typeHash = typeHash;
        TilesetBox.Text = tilesetPath;
        LoadState();
        BuildPalette();
        UpdatePaletteHint();
    }

    // --- 反射属性读写（与 PropertyModel 同款固定指针方式）---

    private unsafe int GetPropInt(string name)
    {
        int v = 0;
        ComponentAPI.GComponent_GetProperty(_entity, _typeHash, name, (nint)(&v), sizeof(int));
        return v;
    }

    private unsafe float GetPropFloat(string name)
    {
        float v = 0;
        ComponentAPI.GComponent_GetProperty(_entity, _typeHash, name, (nint)(&v), sizeof(float));
        return v;
    }

    private unsafe void SetPropInt(string name, int value)
    {
        ComponentAPI.GComponent_SetProperty(_entity, _typeHash, name, (nint)(&value), sizeof(int));
    }

    private unsafe void SetPropFloat(string name, float value)
    {
        ComponentAPI.GComponent_SetProperty(_entity, _typeHash, name, (nint)(&value), sizeof(float));
    }

    private unsafe void SetPropString(string name, string value)
    {
        var buffer = new byte[256];
        int count = Encoding.UTF8.GetBytes(value ?? string.Empty, 0, Math.Min(value?.Length ?? 0, 254), buffer, 0);
        buffer[count] = 0;
        fixed (byte* pb = buffer)
        {
            ComponentAPI.GComponent_SetProperty(_entity, _typeHash, name, (nint)pb, buffer.Length);
        }
    }

    // --- 状态加载 / 画布 ---

    private void LoadState()
    {
        _width = Math.Max(GetPropInt("map_width"), 0);
        _height = Math.Max(GetPropInt("map_height"), 0);
        WidthBox.Text = _width.ToString();
        HeightBox.Text = _height.ToString();
        CellWBox.Text = GetPropFloat("cell_width").ToString("0.##");
        CellHBox.Text = GetPropFloat("cell_height").ToString("0.##");

        int needed = ComponentAPI.GComponent_TilemapGetTiles(_entity, _typeHash, null!, 0);
        _tiles = new int[Math.Max(needed, 0)];
        if (needed > 0)
        {
            ComponentAPI.GComponent_TilemapGetTiles(_entity, _typeHash, _tiles, _tiles.Length);
        }
        RebuildGrid();
    }

    private void RebuildGrid()
    {
        TileGrid.Children.Clear();
        for (int i = 0; i < _tiles.Length; i++)
        {
            int index = i;
            var btn = new Button
            {
                Width = 24,
                Height = 24,
                Margin = new Thickness(1),
                Padding = new Thickness(0),
                FontSize = 9,
                Background = TileBrush(_tiles[i]),
                Content = _tiles[i] < 0 ? "" : _tiles[i].ToString()
            };
            btn.Click += (_, _) => Paint(index, _selectedIndex);
            btn.MouseRightButtonDown += (_, _) => Paint(index, -1);
            TileGrid.Children.Add(btn);
        }
    }

    private void Paint(int index, int tile)
    {
        if (index < 0 || index >= _tiles.Length) return;
        _tiles[index] = tile;
        if (index < TileGrid.Children.Count && TileGrid.Children[index] is Button b)
        {
            b.Background = TileBrush(tile);
            b.Content = tile < 0 ? "" : tile.ToString();
        }
    }

    private static SolidColorBrush TileBrush(int tile)
    {
        if (tile < 0) return new SolidColorBrush(Color.FromArgb(24, 255, 255, 255));
        return new SolidColorBrush(PaletteColors[tile % PaletteColors.Length]);
    }

    private void BuildPalette()
    {
        for (int i = 0; i < PaletteColors.Length; i++)
        {
            int idx = i;
            var btn = new Button
            {
                Width = 26,
                Height = 26,
                Margin = new Thickness(2),
                Padding = new Thickness(0),
                FontSize = 9,
                Background = new SolidColorBrush(PaletteColors[i]),
                Content = i.ToString(),
                ToolTip = "Tile " + i
            };
            btn.Click += (_, _) =>
            {
                _selectedIndex = idx;
                UpdatePaletteHint();
            };
            PalettePanel.Items.Add(btn);
        }
    }

    private void UpdatePaletteHint()
    {
        PaletteHint.Text = "Index: " + _selectedIndex;
    }

    // --- 事件 ---

    private void OnBrowseTilesetClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Title = LocalizationService.Instance.T("tilemap_editor.tileset"),
            Filter = "Tileset (*.json)|*.json|All Files (*.*)|*.*"
        };
        if (dialog.ShowDialog() == true)
        {
            TilesetBox.Text = dialog.FileName;
        }
    }

    private void OnApplySizeClick(object sender, RoutedEventArgs e)
    {
        if (!int.TryParse(WidthBox.Text, out int w) || !int.TryParse(HeightBox.Text, out int h) ||
            w < 0 || h < 0 || w > 2048 || h > 2048 || (long)w * h > (1 << 20))
        {
            MessageBox.Show(this, LocalizationService.Instance.T("tilemap_editor.invalid_size"),
                            LocalizationService.Instance.T("tilemap_editor.title"),
                            MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var newTiles = new int[w * h];
        for (int i = 0; i < newTiles.Length; i++) newTiles[i] = -1;
        for (int y = 0; y < Math.Min(h, _height); y++)
        {
            for (int x = 0; x < Math.Min(w, _width); x++)
            {
                int src = y * _width + x;
                if (src < _tiles.Length) newTiles[y * w + x] = _tiles[src];
            }
        }
        _tiles = newTiles;
        _width = w;
        _height = h;
        RebuildGrid();
    }

    private void OnApplyClick(object sender, RoutedEventArgs e)
    {
        if (_width * _height != _tiles.Length)
        {
            OnApplySizeClick(sender, e);
        }
        SetPropInt("map_width", _width);
        SetPropInt("map_height", _height);
        if (float.TryParse(CellWBox.Text, out float cw)) SetPropFloat("cell_width", cw);
        if (float.TryParse(CellHBox.Text, out float ch)) SetPropFloat("cell_height", ch);
        SetPropString("tileset_path", TilesetBox.Text);
        ComponentAPI.GComponent_TilemapSetTiles(_entity, _typeHash, _tiles, _tiles.Length);

        App.Engine?.MarkSceneDirty();
        App.EditorVM?.Notify("Tilemap updated.");
        DialogResult = true;
    }

    private void OnCloseClick(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }
}
