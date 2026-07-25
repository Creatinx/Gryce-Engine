#!/usr/bin/env python3
"""
PromoCreator - 游戏引擎宣传片自动生成器
用法: python PromoCreator.py config.md [--template default] [--engine "GryceEngine.exe"]
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import List, Dict, Optional

import numpy as np
from scipy.io import wavfile

DEFAULT_RESOLUTION = (1920, 1080)
DEFAULT_FPS = 30
DEFAULT_BPM = 120


class SceneParser:
    """解析 Markdown 文案，提取场景信息"""
    
    def __init__(self, md_path: str):
        self.md_path = md_path
        self.scenes: List[Dict] = []
        self.meta: Dict = {}
        self._parse()
    
    def _parse(self):
        with open(self.md_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        if content.startswith('---'):
            parts = content.split('---', 2)
            if len(parts) >= 3:
                self._parse_meta(parts[1])
                content = parts[2]
        
        scene_blocks = re.split(r'\n## ', content)
        
        for block in scene_blocks[1:]:
            scene = self._parse_scene_block(block)
            if scene:
                self.scenes.append(scene)
    
    def _parse_meta(self, yaml_text: str):
        for line in yaml_text.strip().split('\n'):
            if ':' in line:
                k, v = line.split(':', 1)
                self.meta[k.strip()] = v.strip()
    
    def _parse_scene_block(self, block: str) -> Optional[Dict]:
        lines = block.strip().split('\n')
        if not lines:
            return None
        
        title = lines[0].strip()
        scene = {"title": title, "type": "feature", "duration": 4.0,
                 "record": None, "camera": "static", "text": "",
                 "subtitle": "", "bgm_intensity": 0.5}
        
        for line in lines[1:]:
            line = line.strip()
            if line.startswith('type:'):
                scene["type"] = line.split(':', 1)[1].strip()
            elif line.startswith('duration:'):
                scene["duration"] = float(line.split(':', 1)[1].strip())
            elif line.startswith('record:'):
                scene["record"] = line.split(':', 1)[1].strip()
            elif line.startswith('camera:'):
                scene["camera"] = line.split(':', 1)[1].strip()
            elif line.startswith('text:'):
                scene["text"] = line.split(':', 1)[1].strip()
            elif line.startswith('subtitle:'):
                scene["subtitle"] = line.split(':', 1)[1].strip()
            elif line.startswith('bgm_intensity:'):
                scene["bgm_intensity"] = float(line.split(':', 1)[1].strip())
            elif line.startswith('description:'):
                scene["description"] = line.split(':', 1)[1].strip()
        
        return scene


class TemplateEngine:
    def __init__(self, template_path: str):
        with open(template_path, 'r', encoding='utf-8') as f:
            self.config = json.load(f)
    
    def get_style(self, scene_type: str) -> Dict:
        return self.config.get("styles", {}).get(scene_type, self.config.get("styles", {}).get("default", {}))
    
    def get_transition(self) -> str:
        return self.config.get("transition", "fade")
    
    def get_bgm_config(self) -> Dict:
        return self.config.get("bgm", {"bpm": 120, "style": "minimal_electronic"})


class EngineRecorder:
    def __init__(self, engine_path: str, output_dir: str):
        self.engine_path = engine_path
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)
    
    def record_scene(self, scene_name: str, duration: float, camera: str,
                     resolution: str = "1920x1080") -> str:
        output_path = os.path.join(self.output_dir, f"{scene_name.replace('/', '_')}.mp4")
        
        cmd = [
            self.engine_path,
            "--scene", scene_name,
            "--record", str(duration),
            "--camera", camera,
            "--resolution", resolution,
            "--output", output_path,
            "--no-audio"
        ]
        
        print(f"[录制] {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            print(f"[警告] 录制失败: {result.stderr}")
            return None
        
        if not os.path.exists(output_path):
            print(f"[警告] 引擎未生成文件: {output_path}")
            return None
        
        print(f"[完成] 素材已保存: {output_path}")
        return output_path
    
    def screenshot(self, scene_name: str, camera: str, output_name: str) -> str:
        output_path = os.path.join(self.output_dir, f"{output_name}.png")
        cmd = [
            self.engine_path,
            "--scene", scene_name,
            "--screenshot", output_path,
            "--camera", camera,
            "--resolution", "1920x1080"
        ]
        print(f"[截图] {' '.join(cmd)}")
        subprocess.run(cmd, capture_output=True, text=True)
        return output_path if os.path.exists(output_path) else None


class BGMGenerator:
    def __init__(self, bpm: int = 120, sample_rate: int = 44100):
        self.bpm = bpm
        self.sample_rate = sample_rate
        self.beat_duration = 60.0 / bpm
    
    def generate(self, scenes: List[Dict], output_path: str):
        total_duration = sum(s["duration"] for s in scenes)
        t = np.linspace(0, total_duration, int(self.sample_rate * total_duration), False)
        
        audio_left = np.zeros_like(t)
        audio_right = np.zeros_like(t)
        
        audio_left = self._add_pad(t, audio_left, 82.4, 0.06)
        audio_right = self._add_pad(t, audio_right, 98.0, 0.05)
        audio_right = self._add_pad(t, audio_right, 123.5, 0.03)
        
        current_time = 0.0
        for scene in scenes:
            dur = scene["duration"]
            intensity = scene.get("bgm_intensity", 0.5)
            num_beats = int(dur / self.beat_duration) + 1
            
            for i in range(num_beats):
                beat_time = current_time + i * self.beat_duration
                if beat_time >= total_duration:
                    break
                
                audio_left = self._add_kick(t, audio_left, beat_time, 0.6 * intensity)
                audio_left = self._add_hihat(t, audio_left, beat_time, 0.2 * intensity)
                audio_left = self._add_hihat(t, audio_left, beat_time + self.beat_duration/2, 0.15 * intensity)
                
                if i % 2 == 0:
                    freq = 55 if (i % 4 == 0) else 65.4
                    audio_left = self._add_bass(t, audio_left, beat_time,
                                                self.beat_duration * 1.8, freq, 0.2 * intensity)
            
            next_time = current_time + dur
            if next_time < total_duration:
                audio_left = self._add_whoosh(t, audio_left, next_time, 0.25)
                audio_right = self._add_whoosh(t, audio_right, next_time, 0.25)
            
            current_time += dur
        
        audio_left = self._add_subdrop(t, audio_left, current_time - 1.0, 0.7)
        audio_right = self._add_subdrop(t, audio_right, current_time - 1.0, 0.7)
        
        stereo = np.stack([audio_left, audio_right], axis=-1)
        max_val = np.max(np.abs(stereo))
        if max_val > 0:
            stereo = stereo / max_val * 0.85
        stereo_int16 = (stereo * 32767).astype(np.int16)
        
        wavfile.write(output_path, self.sample_rate, stereo_int16)
        print(f"[BGM] 已生成: {output_path} ({total_duration:.1f}s)")
    
    def _add_kick(self, t_arr, audio, time_pos, intensity=1.0):
        mask = (t_arr >= time_pos) & (t_arr < time_pos + 0.15)
        if not np.any(mask): return audio
        lt = t_arr[mask] - time_pos
        freq = 150 * np.exp(-lt * 30)
        env = np.exp(-lt * 20)
        audio[mask] += np.sin(2 * np.pi * freq * lt) * env * 0.5 * intensity
        return audio
    
    def _add_hihat(self, t_arr, audio, time_pos, intensity=0.3):
        mask = (t_arr >= time_pos) & (t_arr < time_pos + 0.05)
        if not np.any(mask): return audio
        lt = t_arr[mask] - time_pos
        noise = np.random.uniform(-1, 1, size=lt.shape)
        env = np.exp(-lt * 80)
        audio[mask] += noise * env * intensity
        return audio
    
    def _add_bass(self, t_arr, audio, time_pos, duration, freq, intensity):
        mask = (t_arr >= time_pos) & (t_arr < time_pos + duration)
        if not np.any(mask): return audio
        lt = t_arr[mask] - time_pos
        env = np.exp(-lt * 3)
        audio[mask] += np.sign(np.sin(2 * np.pi * freq * lt)) * env * intensity
        return audio
    
    def _add_pad(self, t_arr, audio, freq, intensity):
        pad = np.sin(2 * np.pi * freq * t_arr) * intensity
        pad += np.sin(2 * np.pi * freq * 1.5 * t_arr) * intensity * 0.5
        pad += np.sin(2 * np.pi * freq * 2 * t_arr) * intensity * 0.3
        return audio + pad
    
    def _add_whoosh(self, t_arr, audio, time_pos, intensity=0.4):
        duration = 0.3
        mask = (t_arr >= time_pos) & (t_arr < time_pos + duration)
        if not np.any(mask): return audio
        lt = t_arr[mask] - time_pos
        freq = np.clip(2000 - lt * 5000, 200, 4000)
        env = np.sin(np.pi * lt / duration) * intensity
        audio[mask] += np.sin(2 * np.pi * freq * lt) * env
        return audio
    
    def _add_subdrop(self, t_arr, audio, time_pos, intensity=0.8):
        duration = 0.8
        mask = (t_arr >= time_pos) & (t_arr < time_pos + duration)
        if not np.any(mask): return audio
        lt = t_arr[mask] - time_pos
        freq = 100 * np.exp(-lt * 8)
        env = np.exp(-lt * 4)
        audio[mask] += np.sin(2 * np.pi * freq * lt) * env * intensity
        return audio


class VideoComposer:
    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)
    
    def compose(self, scene_clips: List[str], bgm_path: str,
                transitions: List[str], output_name: str = "trailer_final.mp4"):
        if not scene_clips:
            print("[错误] 没有素材可合成")
            return None
        
        processed = []
        for i, clip in enumerate(scene_clips):
            if clip is None or not os.path.exists(clip):
                continue
            out = os.path.join(self.output_dir, f"proc_{i:02d}.mp4")
            fade_dur = 0.3
            
            probe = subprocess.run([
                'ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                '-of', 'default=noprint_wrappers=1:nokey=1', clip
            ], capture_output=True, text=True)
            dur = float(probe.stdout.strip())
            fade_out_start = max(0, dur - fade_dur)
            
            vf = f"fade=t=in:st=0:d={fade_dur},fade=t=out:st={fade_out_start}:d={fade_dur}"
            af = f"afade=t=in:st=0:d={fade_dur},afade=t=out:st={fade_out_start}:d={fade_dur}"
            
            cmd = ['ffmpeg', '-y', '-i', clip, '-vf', vf, '-af', af,
                   '-c:v', 'libx264', '-preset', 'fast', '-crf', '23',
                   '-c:a', 'aac', '-b:a', '128k', out]
            subprocess.run(cmd, capture_output=True)
            processed.append(out)
        
        if not processed:
            return None
        
        concat_list = os.path.join(self.output_dir, "concat.txt")
        with open(concat_list, 'w') as f:
            for p in processed:
                f.write(f"file '{p}'\n")
        
        concat_video = os.path.join(self.output_dir, "concat.mp4")
        subprocess.run(['ffmpeg', '-y', '-f', 'concat', '-safe', '0',
                        '-i', concat_list, '-c', 'copy', concat_video],
                       capture_output=True)
        
        final_path = os.path.join(self.output_dir, output_name)
        cmd = ['ffmpeg', '-y', '-i', concat_video, '-i', bgm_path,
               '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k',
               '-shortest', '-map', '0:v:0', '-map', '1:a:0',
               final_path]
        subprocess.run(cmd, capture_output=True)
        
        print(f"[完成] 宣传片已生成: {final_path}")
        return final_path


def main():
    parser = argparse.ArgumentParser(description="PromoCreator - 游戏引擎宣传片自动生成器")
    parser.add_argument("config", help="Markdown 文案文件路径")
    parser.add_argument("--engine", default="GryceEngine.exe", help="引擎可执行文件路径")
    parser.add_argument("--template", default="default", help="模板名称")
    parser.add_argument("--output-dir", default="./PromoCreator_output", help="输出目录")
    parser.add_argument("--skip-record", action="store_true", help="跳过录制，只合成")
    args = parser.parse_args()
    
    print(f"[1/5] 解析文案: {args.config}")
    md = SceneParser(args.config)
    print(f"      发现 {len(md.scenes)} 个场景")
    
    template_dir = Path(__file__).parent / "templates"
    template_path = template_dir / f"{args.template}.json"
    if not template_path.exists():
        print(f"[错误] 模板不存在: {template_path}")
        sys.exit(1)
    template = TemplateEngine(str(template_path))
    print(f"[2/5] 加载模板: {args.template}")
    
    clips_dir = os.path.join(args.output_dir, "clips")
    recorder = EngineRecorder(args.engine, clips_dir)
    scene_clips = []
    
    if not args.skip_record:
        print(f"[3/5] 开始录制素材...")
        for scene in md.scenes:
            if scene.get("record"):
                clip = recorder.record_scene(
                    scene["record"],
                    scene["duration"],
                    scene.get("camera", "static")
                )
                scene_clips.append(clip)
            else:
                print(f"      场景 '{scene['title']}' 无录制参数，跳过")
                scene_clips.append(None)
    else:
        print(f"[3/5] 跳过录制，使用已有素材")
        for scene in md.scenes:
            if scene.get("record"):
                path = os.path.join(clips_dir, f"{scene['record'].replace('/', '_')}.mp4")
                scene_clips.append(path if os.path.exists(path) else None)
            else:
                scene_clips.append(None)
    
    print(f"[4/5] 生成BGM...")
    bgm_path = os.path.join(args.output_dir, "bgm.wav")
    bgm_gen = BGMGenerator(bpm=template.get_bgm_config().get("bpm", 120))
    bgm_gen.generate(md.scenes, bgm_path)
    
    print(f"[5/5] 合成最终视频...")
    composer = VideoComposer(args.output_dir)
    final = composer.compose(scene_clips, bgm_path,
                             [template.get_transition()] * len(scene_clips))
    
    if final:
        print(f"\n🎬 宣传片生成完成: {final}")
    else:
        print(f"\n❌ 生成失败")


if __name__ == "__main__":
    main()