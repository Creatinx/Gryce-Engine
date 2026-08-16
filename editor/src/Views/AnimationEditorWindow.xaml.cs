using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace GryceEngine.Editor.Views;

/// <summary>
/// 动画 / 骨骼动画编辑器：播放控制（片段、循环、速度、时间轴）+ 骨骼层级浏览 +
/// 片段骨骼轨道的关键帧查看（数据来自模型资源，只读）+ 导出 .anim.json。
/// 关键帧编辑需要模型可写/保存链路，后续版本接入。
/// </summary>
public partial class AnimationEditorWindow : Window
{
    public sealed class ClipItem
    {
        public int Index { get; init; }
        public string Name { get; init; } = "";
        public float Duration { get; init; }
    }

    public sealed class BoneNode
    {
        public int Index { get; init; }
        public string Name { get; init; } = "";
        public List<BoneNode> Children { get; } = new();
    }

    public sealed class TrackItem
    {
        public int TrackIndex { get; init; }
        public int BoneIndex { get; init; }
        public string BoneName { get; init; } = "";
        public int PosKeys { get; init; }
        public int RotKeys { get; init; }
        public int SclKeys { get; init; }
        public string TrackInfo => $"P:{PosKeys}  R:{RotKeys}  S:{SclKeys}";
    }

    public sealed class KeyframeRow
    {
        public string Channel { get; init; } = "";
        public int KeyIndex { get; init; }
        public float Time { get; init; }
        public float X { get; init; }
        public float Y { get; init; }
        public float Z { get; init; }
        public float W { get; init; }
    }

    private readonly GEntityHandle _entity;
    private readonly ulong _compHash;
    private readonly DispatcherTimer _playbackTimer = new(DispatcherPriority.Render)
    {
        Interval = TimeSpan.FromMilliseconds(33)
    };
    private bool _syncing;
    private bool _playing;

    public AnimationEditorWindow(GEntityHandle entity, ulong compHash, string entityName)
    {
        InitializeComponent();
        _entity = entity;
        _compHash = compHash;
        EntityLabel.Text = $"{LocalizationService.Instance.T("animation_editor.entity")}: {entityName}";
        _playbackTimer.Tick += OnPlaybackTick;
        _playbackTimer.Start();
        Closed += (_, _) => _playbackTimer.Stop();
        RefreshAll();
    }

    private static string T(string key) => LocalizationService.Instance.T(key);

    // === 刷新 ===

    private void RefreshAll()
    {
        RefreshClips();
        RefreshSkeleton();
        RefreshPlaybackState();
    }

    private void OnRefreshClick(object sender, RoutedEventArgs e) => RefreshAll();

    private void RefreshClips()
    {
        var items = new List<ClipItem>();
        int count = AnimatorAPI.GAnimator_GetClipCount(_entity, _compHash);
        for (int i = 0; i < count; i++)
        {
            string name = AnimatorAPI.GetClipNameUtf8(_entity, _compHash, i) ?? $"Clip{i}";
            float dur = Math.Max(AnimatorAPI.GAnimator_GetClipDuration(_entity, _compHash, i), 0f);
            items.Add(new ClipItem { Index = i, Name = name, Duration = dur });
        }

        NoClipsText.Visibility = count > 0 ? Visibility.Collapsed : Visibility.Visible;
        ClipList.Visibility = count > 0 ? Visibility.Visible : Visibility.Collapsed;
        ClipList.ItemsSource = items;

        if (items.Count > 0)
        {
            string current = GetPropString("clip_name") ?? string.Empty;
            int idx = items.FindIndex(c => c.Name == current);
            ClipList.SelectedIndex = idx >= 0 ? idx : 0;
        }
        RefreshTracks();
    }

    private void RefreshSkeleton()
    {
        int boneCount = AnimatorAPI.GAnimator_GetBoneCount(_entity, _compHash);
        NoSkeletonText.Visibility = boneCount > 0 ? Visibility.Collapsed : Visibility.Visible;
        BoneTree.Visibility = boneCount > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (boneCount <= 0)
        {
            BoneTree.Items.Clear();
            return;
        }

        var nodes = new BoneNode[boneCount];
        for (int i = 0; i < boneCount; i++)
        {
            nodes[i] = new BoneNode
            {
                Index = i,
                Name = AnimatorAPI.GetBoneNameUtf8(_entity, _compHash, i) ?? $"Bone{i}"
            };
        }
        var roots = new List<BoneNode>();
        for (int i = 0; i < boneCount; i++)
        {
            int parent = AnimatorAPI.GAnimator_GetBoneParentIndex(_entity, _compHash, i);
            if (parent >= 0 && parent < boneCount) nodes[parent].Children.Add(nodes[i]);
            else roots.Add(nodes[i]);
        }

        BoneTree.Items.Clear();
        foreach (var root in roots) BoneTree.Items.Add(CreateBoneItem(root));
    }

    private static TreeViewItem CreateBoneItem(BoneNode node)
    {
        var item = new TreeViewItem
        {
            Header = $"[{node.Index}]  {node.Name}",
            IsExpanded = true
        };
        foreach (var child in node.Children) item.Items.Add(CreateBoneItem(child));
        return item;
    }

    private void RefreshPlaybackState()
    {
        _syncing = true;
        try
        {
            LoopCheck.IsChecked = GetPropFloat("loop") > 0.5f;
            float speed = GetPropFloat("speed");
            SpeedSlider.Value = speed;
            SpeedLabel.Text = $"{speed:0.00}x";
            _playing = GetPropFloat("playing") > 0.5f;
            UpdatePlayButton();
            TimeSlider.Maximum = CurrentDuration() > 0 ? CurrentDuration() : 10;
            TimeSlider.Value = Math.Max(0f, GetPropFloat("time"));
            UpdateTimeLabel();
        }
        finally
        {
            _syncing = false;
        }
    }

    private float CurrentDuration()
    {
        if (ClipList.SelectedItem is ClipItem clip) return clip.Duration;
        return 0f;
    }

    private void RefreshTracks()
    {
        var items = new List<TrackItem>();
        if (ClipList.SelectedItem is ClipItem clip)
        {
            int trackCount = AnimatorAPI.GAnimator_GetClipTrackCount(_entity, _compHash, clip.Index);
            for (int t = 0; t < trackCount; t++)
            {
                int bone = AnimatorAPI.GAnimator_GetClipTrackBone(_entity, _compHash, clip.Index, t);
                string boneName = bone >= 0
                    ? AnimatorAPI.GetBoneNameUtf8(_entity, _compHash, bone) ?? $"Bone{bone}"
                    : "?";
                items.Add(new TrackItem
                {
                    TrackIndex = t,
                    BoneIndex = bone,
                    BoneName = boneName,
                    PosKeys = Math.Max(AnimatorAPI.GAnimator_GetClipKeyframeCount(_entity, _compHash, clip.Index, t, 0), 0),
                    RotKeys = Math.Max(AnimatorAPI.GAnimator_GetClipKeyframeCount(_entity, _compHash, clip.Index, t, 1), 0),
                    SclKeys = Math.Max(AnimatorAPI.GAnimator_GetClipKeyframeCount(_entity, _compHash, clip.Index, t, 2), 0)
                });
            }
        }
        TrackCombo.ItemsSource = items;
        TrackCombo.SelectedIndex = items.Count > 0 ? 0 : -1;
        RefreshKeyframes();
    }

    private void RefreshKeyframes()
    {
        var rows = new List<KeyframeRow>();
        if (ClipList.SelectedItem is ClipItem clip && TrackCombo.SelectedItem is TrackItem track)
        {
            AddChannelRows(rows, clip.Index, track.TrackIndex, 0, T("animation_editor.chan_pos"));
            AddChannelRows(rows, clip.Index, track.TrackIndex, 1, T("animation_editor.chan_rot"));
            AddChannelRows(rows, clip.Index, track.TrackIndex, 2, T("animation_editor.chan_scl"));
        }
        KeyframeGrid.ItemsSource = rows;
        NoKeyframeText.Visibility = rows.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        KeyframeGrid.Visibility = rows.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void AddChannelRows(List<KeyframeRow> rows, int clipIndex, int trackIndex, int channel, string label)
    {
        int count = AnimatorAPI.GAnimator_GetClipKeyframeCount(_entity, _compHash, clipIndex, trackIndex, channel);
        if (count <= 0) return;
        var buf = new float[5];
        for (int k = 0; k < count; k++)
        {
            if (AnimatorAPI.GAnimator_GetClipKeyframe(_entity, _compHash, clipIndex, trackIndex,
                    channel, k, buf, buf.Length) < 0) continue;
            rows.Add(new KeyframeRow
            {
                Channel = label,
                KeyIndex = k,
                Time = buf[0],
                X = buf.Length > 1 ? buf[1] : 0,
                Y = buf.Length > 2 ? buf[2] : 0,
                Z = buf.Length > 3 ? buf[3] : 0,
                W = buf.Length > 4 ? buf[4] : 0
            });
        }
    }

    private void OnClipSelected(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing) return;
        if (ClipList.SelectedItem is ClipItem clip)
        {
            _syncing = true;
            try
            {
                SetPropString("clip_name", clip.Name);
                SetPropFloat("time", 0f);
                TimeSlider.Maximum = clip.Duration > 0 ? clip.Duration : 10;
                TimeSlider.Value = 0;
                UpdateTimeLabel();
            }
            finally { _syncing = false; }
            App.Engine?.MarkSceneDirty();
        }
        RefreshTracks();
        RefreshPlaybackState();
    }

    private void OnTrackSelected(object sender, SelectionChangedEventArgs e) => RefreshKeyframes();

    // === 播放 ===

    private void OnPlayPauseClick(object sender, RoutedEventArgs e)
    {
        _playing = !_playing;
        SetPropFloat("playing", _playing ? 1f : 0f);
        UpdatePlayButton();
        App.Engine?.MarkSceneDirty();
    }

    private void OnStopClick(object sender, RoutedEventArgs e)
    {
        _playing = false;
        SetPropFloat("playing", 0f);
        SetPropFloat("time", 0f);
        _syncing = true;
        try { TimeSlider.Value = 0; } finally { _syncing = false; }
        UpdatePlayButton();
        UpdateTimeLabel();
        App.Engine?.MarkSceneDirty();
    }

    private void OnLoopChanged(object sender, RoutedEventArgs e)
    {
        if (_syncing) return;
        SetPropFloat("loop", LoopCheck.IsChecked == true ? 1f : 0f);
        App.Engine?.MarkSceneDirty();
    }

    private void OnSpeedChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_syncing) return;
        SpeedLabel.Text = $"{SpeedSlider.Value:0.00}x";
        SetPropFloat("speed", (float)SpeedSlider.Value);
        App.Engine?.MarkSceneDirty();
    }

    private void OnTimeChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_syncing) return;
        SetPropFloat("time", (float)TimeSlider.Value);
        UpdateTimeLabel();
    }

    private void OnPlaybackTick(object? sender, EventArgs e)
    {
        if (!_playing || _syncing) return;
        float dt = 0.033f;
        float speed = Math.Max(GetPropFloat("speed"), 0f);
        float duration = CurrentDuration();
        float time = GetPropFloat("time") + speed * dt;
        bool loop = GetPropFloat("loop") > 0.5f;

        if (duration > 0f && time >= duration)
        {
            if (loop) time %= duration;
            else
            {
                time = duration;
                _playing = false;
                SetPropFloat("playing", 0f);
                UpdatePlayButton();
            }
        }

        SetPropFloat("time", time);
        _syncing = true;
        try
        {
            TimeSlider.Maximum = duration > 0 ? duration : 10;
            TimeSlider.Value = time;
        }
        finally { _syncing = false; }
        UpdateTimeLabel();
    }

    private void UpdatePlayButton()
    {
        PlayPauseButton.Content = _playing ? "\uE769" : "\uE768";
        PlayPauseButton.ToolTip = _playing ? T("animation_editor.pause") : T("animation_editor.play");
    }

    private void UpdateTimeLabel()
    {
        float duration = CurrentDuration();
        TimeLabel.Text = $"{Math.Max(0f, GetPropFloat("time")):0.00}s / {(duration > 0 ? duration : 0):0.00}s";
    }

    // === 导出 ===

    private void OnExportClick(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.SaveFileDialog
        {
            Title = T("animation_editor.export_title"),
            Filter = T("animation_editor.export_filter"),
            FileName = "animation.anim.json"
        };
        if (dialog.ShowDialog(this) != true) return;

        try
        {
            System.IO.File.WriteAllText(dialog.FileName, BuildExportJson(), new UTF8Encoding(false));
            VMNotify(string.Format(T("animation_editor.exported"), dialog.FileName));
        }
        catch (Exception ex)
        {
            VMNotify(string.Format(T("animation_editor.export_failed"), ex.Message), true);
        }
    }

    private string BuildExportJson()
    {
        var sb = new StringBuilder();
        sb.Append("{\n  \"skeleton\": [");
        int boneCount = AnimatorAPI.GAnimator_GetBoneCount(_entity, _compHash);
        for (int i = 0; i < boneCount; i++)
        {
            if (i > 0) sb.Append(',');
            string name = AnimatorAPI.GetBoneNameUtf8(_entity, _compHash, i) ?? $"Bone{i}";
            int parent = AnimatorAPI.GAnimator_GetBoneParentIndex(_entity, _compHash, i);
            sb.Append($"\n    {{\"index\":{i},\"name\":{Json(name)},\"parent\":{parent}}}");
        }
        sb.Append("\n  ],\n  \"clips\": [");

        int clipCount = AnimatorAPI.GAnimator_GetClipCount(_entity, _compHash);
        for (int c = 0; c < clipCount; c++)
        {
            if (c > 0) sb.Append(',');
            string clipName = AnimatorAPI.GetClipNameUtf8(_entity, _compHash, c) ?? $"Clip{c}";
            float dur = Math.Max(AnimatorAPI.GAnimator_GetClipDuration(_entity, _compHash, c), 0f);
            sb.Append($"\n    {{\"name\":{Json(clipName)},\"duration\":{dur.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture)},\"tracks\":[");
            int trackCount = AnimatorAPI.GAnimator_GetClipTrackCount(_entity, _compHash, c);
            for (int t = 0; t < trackCount; t++)
            {
                if (t > 0) sb.Append(',');
                int bone = AnimatorAPI.GAnimator_GetClipTrackBone(_entity, _compHash, c, t);
                sb.Append($"\n      {{\"bone\":{bone},\"positions\":[");
                AppendChannel(sb, c, t, 0, 4);
                sb.Append("],\"rotations\":[");
                AppendChannel(sb, c, t, 1, 5);
                sb.Append("],\"scales\":[");
                AppendChannel(sb, c, t, 2, 4);
                sb.Append("]}");
            }
            sb.Append("\n    ]}");
        }
        sb.Append("\n  ]\n}\n");
        return sb.ToString();
    }

    private void AppendChannel(StringBuilder sb, int clipIndex, int trackIndex, int channel, int stride)
    {
        int count = AnimatorAPI.GAnimator_GetClipKeyframeCount(_entity, _compHash, clipIndex, trackIndex, channel);
        var buf = new float[5];
        for (int k = 0; k < count; k++)
        {
            if (k > 0) sb.Append(',');
            if (AnimatorAPI.GAnimator_GetClipKeyframe(_entity, _compHash, clipIndex, trackIndex,
                    channel, k, buf, buf.Length) < 0) continue;
            sb.Append('{').Append("\"t\":").Append(F(buf[0]));
            for (int i = 1; i < stride; i++)
            {
                sb.Append(',').Append(i == 1 ? "\"x\":" : i == 2 ? "\"y\":" : i == 3 ? "\"z\":" : "\"w\":")
                  .Append(F(buf[i]));
            }
            sb.Append('}');
        }
    }

    private static string F(float v) => v.ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);

    private static string Json(string s) => "\"" + (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";

    private void VMNotify(string message, bool isError = false)
    {
        App.EditorVM?.AppendConsole(message);
        if (isError) App.EditorVM?.Notify(message, true);
    }

    // === 组件属性读写（与 Inspector 反射属性同款）===

    private float GetPropFloat(string name)
    {
        var buf = new byte[4];
        var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try
        {
            if (ComponentAPI.GComponent_GetProperty(_entity, _compHash, name,
                    pin.AddrOfPinnedObject(), 4) == 0)
            {
                return BitConverter.ToSingle(buf, 0);
            }
        }
        finally { pin.Free(); }
        return 0f;
    }

    private void SetPropFloat(string name, float value)
    {
        var buf = BitConverter.GetBytes(value);
        var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try { ComponentAPI.GComponent_SetProperty(_entity, _compHash, name, pin.AddrOfPinnedObject(), 4); }
        finally { pin.Free(); }
    }

    private string? GetPropString(string name)
    {
        var buf = new byte[512];
        var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try
        {
            if (ComponentAPI.GComponent_GetProperty(_entity, _compHash, name,
                    pin.AddrOfPinnedObject(), buf.Length) == 0)
            {
                int len = Array.IndexOf(buf, (byte)0);
                return Encoding.UTF8.GetString(buf, 0, len >= 0 ? len : buf.Length);
            }
        }
        finally { pin.Free(); }
        return null;
    }

    private void SetPropString(string name, string value)
    {
        var buf = new byte[512];
        int count = Encoding.UTF8.GetBytes(value ?? string.Empty, 0,
            Math.Min((value ?? string.Empty).Length, 511), buf, 0);
        buf[count] = 0;
        var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try { ComponentAPI.GComponent_SetProperty(_entity, _compHash, name, pin.AddrOfPinnedObject(), buf.Length); }
        finally { pin.Free(); }
    }
}
