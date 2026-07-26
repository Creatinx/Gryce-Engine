#!/usr/bin/env python3
"""
PromoCreator - 游戏引擎宣传片自动生成器
支持模板驱动的视觉渲染（Pillow + FFmpeg overlay）
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Dict, Optional, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.io import wavfile

DEFAULT_WIDTH = 1920
DEFAULT_HEIGHT = 1080
DEFAULT_FPS = 30


# ============================================================
# Markdown 解析器
# ============================================================
class SceneParser:
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
            if ':' in line and not line.strip().startswith('#'):
                k, v = line.split(':', 1)
                self.meta[k.strip()] = v.strip()

    def _parse_scene_block(self, block: str) -> Optional[Dict]:
        lines = block.strip().split('\n')
        if not lines:
            return None

        title = lines[0].strip()
        scene = {
            "title": title,
            "type": "feature",
            "duration": 4.0,
            "record": None,
            "camera": "static",
            "text": "",
            "subtitle": "",
            "heading": "",
            "description": "",
            "bgm_intensity": 0.5,
            "items": [],
            "cards": [],
            "layout": "auto",
        }

        in_cards = False

        for line in lines[1:]:
            line_stripped = line.strip()
            if not line_stripped:
                continue

            if line_stripped.startswith('type:'):
                scene["type"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('duration:'):
                scene["duration"] = float(line_stripped.split(':', 1)[1].strip())
            elif line_stripped.startswith('record:'):
                scene["record"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('camera:'):
                scene["camera"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('text:'):
                scene["text"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('subtitle:'):
                scene["subtitle"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('heading:'):
                scene["heading"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('bgm_intensity:'):
                scene["bgm_intensity"] = float(line_stripped.split(':', 1)[1].strip())
            elif line_stripped.startswith('description:'):
                scene["description"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('layout:'):
                scene["layout"] = line_stripped.split(':', 1)[1].strip()
            elif line_stripped.startswith('cards:'):
                in_cards = True
            elif in_cards and line_stripped.startswith('- '):
                card_text = line_stripped[2:].strip()
                if '|' in card_text:
                    parts = [p.strip() for p in card_text.split('|')]
                    card = {"title": parts[0], "subtitle": parts[1] if len(parts) > 1 else "", "desc": parts[2] if len(parts) > 2 else ""}
                else:
                    card = {"title": card_text, "subtitle": "", "desc": ""}
                scene["cards"].append(card)
            elif line_stripped.startswith('- ') or line_stripped.startswith('▪ '):
                in_cards = False
                item_text = line_stripped[2:].strip()
                scene["items"].append(item_text)
            else:
                in_cards = False

        if not scene["heading"]:
            scene["heading"] = scene["title"]

        return scene


# ============================================================
# 模板引擎
# ============================================================
class TemplateEngine:
    def __init__(self, template_path: str):
        with open(template_path, 'r', encoding='utf-8') as f:
            self.config = json.load(f)

    def get(self, key: str, default=None):
        keys = key.split('.')
        val = self.config
        for k in keys:
            val = val.get(k, default) if isinstance(val, dict) else default
            if val is None:
                return default
        return val

    def get_bgm_config(self) -> Dict:
        return self.get("bgm", {"bpm": 120})


# ============================================================
# 字体加载器
# ============================================================
class FontManager:
    def __init__(self, template: TemplateEngine):
        self.template = template
        self._cache: Dict[str, ImageFont.FreeTypeFont] = {}
        self._find_fonts()

    def _find_fonts(self):
        self.font_paths = {"bold": None, "regular": None, "mono": None}

        bold_candidates = [
            "/usr/share/fonts/truetype/inter/Inter-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
            "/System/Library/Fonts/Helvetica.ttc",
            "C:/Windows/Fonts/arialbd.ttf",
            "C:/Windows/Fonts/msyhbd.ttc",
        ]
        regular_candidates = [
            "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/System/Library/Fonts/Helvetica.ttc",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/msyh.ttc",
        ]
        mono_candidates = [
            "/usr/share/fonts/truetype/jetbrains/JetBrainsMono-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "C:/Windows/Fonts/consola.ttf",
            "C:/Windows/Fonts/cour.ttf",
        ]

        for c in bold_candidates:
            if os.path.exists(c):
                self.font_paths["bold"] = c
                break
        for c in regular_candidates:
            if os.path.exists(c):
                self.font_paths["regular"] = c
                break
        for c in mono_candidates:
            if os.path.exists(c):
                self.font_paths["mono"] = c
                break

    def get(self, font_key: str) -> ImageFont.FreeTypeFont:
        cache_key = font_key
        if cache_key in self._cache:
            return self._cache[cache_key]

        cfg = self.template.get(f"fonts.{font_key}", {})
        size = cfg.get("size", 20)

        if "mono" in font_key or "label" in font_key or "tag" in font_key:
            path = self.font_paths["mono"]
        else:
            path = self.font_paths["bold"] if "title" in font_key or "heading" in font_key else self.font_paths["regular"]

        try:
            if path:
                font = ImageFont.truetype(path, size)
            else:
                font = ImageFont.load_default()
        except Exception:
            font = ImageFont.load_default()

        self._cache[cache_key] = font
        return font


# ============================================================
# 场景渲染器
# ============================================================
class SceneRenderer:
    def __init__(self, template: TemplateEngine, fonts: FontManager, output_dir: str):
        self.template = template
        self.fonts = fonts
        self.output_dir = output_dir
        self.width = template.get("layout.width", 1920)
        self.height = template.get("layout.height", 1080)
        self.bg = template.get("layout.background", "#0A0A0A")
        self.pad_x = template.get("layout.padding_x", 120)
        self.pad_y = template.get("layout.padding_y", 100)

    def _hex_to_rgb(self, hex_color: str) -> Tuple[int, int, int]:
        hex_color = hex_color.lstrip('#')
        return tuple(int(hex_color[i:i+2], 16) for i in (0, 2, 4))

    def _text_size(self, draw: ImageDraw.Draw, text: str, font) -> Tuple[int, int]:
        bbox = draw.textbbox((0, 0), text, font=font)
        return bbox[2] - bbox[0], bbox[3] - bbox[1]

    def render_frame(self, scene: Dict, frame_idx: int, total_frames: int, base_img: Optional[Image.Image] = None) -> Image.Image:
        progress = frame_idx / max(total_frames - 1, 1)

        if base_img:
            img = base_img.copy().convert("RGBA")
        else:
            img = Image.new("RGBA", (self.width, self.height), self._hex_to_rgb(self.bg) + (255,))

        draw = ImageDraw.Draw(img)
        scene_type = scene.get("type", "feature")

        if progress < 0.15:
            fade_alpha = int(255 * (1 - progress / 0.15))
            overlay = Image.new("RGBA", (self.width, self.height), (10, 10, 10, fade_alpha))
            img = Image.alpha_composite(img, overlay)
            draw = ImageDraw.Draw(img)

        if scene_type == "title":
            self._draw_title(draw, scene, progress)
        elif scene_type == "ending":
            self._draw_ending(draw, scene, progress)
        else:
            self._draw_feature(draw, scene, progress)

        return img

    def _draw_title(self, draw: ImageDraw.Draw, scene: Dict, progress: float):
        text = scene.get("text", scene.get("title", ""))
        subtitle = scene.get("subtitle", "")
        mono_text = scene.get("mono", "v0.1.0 · C++23 · MIT · Windows")

        title_font = self.fonts.get("title")
        sub_font = self.fonts.get("subtitle")
        mono_font = self.fonts.get("mono")
        tag_font = self.fonts.get("tag")

        chars_to_show = int(len(text) * min(progress / 0.4, 1.0))
        displayed_text = text[:chars_to_show]

        y = self.pad_y + 80
        x = self.pad_x

        draw.text((x, y), displayed_text, font=title_font, fill=self._hex_to_rgb("#FFFFFF") + (255,))

        if progress < 0.9 and chars_to_show < len(text):
            tw, th = self._text_size(draw, displayed_text, title_font)
            cursor_x = x + tw + 4
            if int(progress * 8) % 2 == 0:
                draw.rectangle([cursor_x, y, cursor_x + 3, y + th], fill=self._hex_to_rgb("#FFFFFF") + (255,))

        if progress > 0.3:
            y += 120
            draw.text((x, y), subtitle, font=sub_font, fill=self._hex_to_rgb("#888888") + (255,))

        if progress > 0.5:
            mono_y = y + 60
            tw, th = self._text_size(draw, mono_text, mono_font)
            box_w = tw + 40
            box_h = th + 24

            draw.rectangle([x, mono_y, x + box_w, mono_y + box_h], fill=self._hex_to_rgb("#0F0F0F") + (255,))
            draw.rectangle([x, mono_y, x + 2, mono_y + box_h], fill=self._hex_to_rgb("#FFFFFF") + (255,))
            draw.rectangle([x, mono_y, x + box_w, mono_y + box_h], outline=self._hex_to_rgb("#222222") + (255,), width=1)
            draw.text((x + 16, mono_y + 10), mono_text, font=mono_font, fill=self._hex_to_rgb("#666666") + (255,))

            if int(progress * 10) % 2 == 0:
                cursor_x = x + 16 + tw + 4
                draw.rectangle([cursor_x, mono_y + 10, cursor_x + 2, mono_y + 10 + th], fill=self._hex_to_rgb("#FFFFFF") + (255,))

        tags = ["OPENGL 4.6", "VULKAN 1.2", "ECS", "JOLT / BOX2D"]
        tag_y = self.height - 80
        tag_x = self.width - self.pad_x

        for tag in reversed(tags):
            tw, th = self._text_size(draw, tag, tag_font)
            tag_x -= tw
            draw.text((tag_x, tag_y), tag, font=tag_font, fill=self._hex_to_rgb("#444444") + (255,))
            tag_x -= 32

    def _draw_ending(self, draw: ImageDraw.Draw, scene: Dict, progress: float):
        text = scene.get("text", scene.get("title", ""))
        subtitle = scene.get("subtitle", "")
        mono_text = "Work in Progress"

        title_font = self.fonts.get("title")
        sub_font = self.fonts.get("subtitle")
        mono_font = self.fonts.get("mono")
        body_font = self.fonts.get("body")

        y = self.pad_y + 80
        x = self.pad_x

        draw.text((x, y), text, font=title_font, fill=self._hex_to_rgb("#FFFFFF") + (255,))
        y += 120
        draw.text((x, y), subtitle, font=sub_font, fill=self._hex_to_rgb("#888888") + (255,))

        mono_y = y + 60
        tw, th = self._text_size(draw, mono_text, mono_font)
        box_w = tw + 40
        box_h = th + 24
        draw.rectangle([x, mono_y, x + box_w, mono_y + box_h], fill=self._hex_to_rgb("#0F0F0F") + (255,))
        draw.rectangle([x, mono_y, x + 2, mono_y + box_h], fill=self._hex_to_rgb("#FFFFFF") + (255,))
        draw.rectangle([x, mono_y, x + box_w, mono_y + box_h], outline=self._hex_to_rgb("#222222") + (255,), width=1)
        draw.text((x + 16, mono_y + 10), mono_text, font=mono_font, fill=self._hex_to_rgb("#666666") + (255,))

        btn_y = mono_y + 100
        buttons = [("GH", "GitHub", "github.com/Creatinx/Gryce-Engine"), ("FD", "爱发电", "ifdian.net/a/creatinx")]

        for btn in buttons:
            prefix, name, url = btn
            btn_w = 380
            btn_h = 52

            draw.rectangle([x, btn_y, x + btn_w, btn_y + btn_h], fill=self._hex_to_rgb("#111111") + (255,))
            draw.rectangle([x, btn_y, x + btn_w, btn_y + btn_h], outline=self._hex_to_rgb("#2A2A2A") + (255,), width=1)

            draw.text((x + 20, btn_y + 14), prefix, font=body_font, fill=self._hex_to_rgb("#888888") + (255,))
            draw.text((x + 60, btn_y + 12), name, font=self.fonts.get("heading"), fill=self._hex_to_rgb("#FFFFFF") + (255,))
            draw.text((x + 160, btn_y + 16), url, font=mono_font, fill=self._hex_to_rgb("#666666") + (255,))

            x += btn_w + 30

        lic_font = self.fonts.get("tag")
        draw.text((self.pad_x, self.height - 60), "MIT License", font=lic_font, fill=self._hex_to_rgb("#333333") + (255,))

    def _draw_feature(self, draw: ImageDraw.Draw, scene: Dict, progress: float):
        label = scene.get("title", "")
        heading = scene.get("heading", "")
        desc = scene.get("description", "")
        items = scene.get("items", [])
        cards = scene.get("cards", [])
        layout = scene.get("layout", "auto")

        label_font = self.fonts.get("label")
        heading_font = self.fonts.get("heading")
        body_font = self.fonts.get("body")

        x = self.pad_x
        y = self.pad_y

        draw.text((x, y), label.upper(), font=label_font, fill=self._hex_to_rgb("#555555") + (255,))
        y += 40

        lines = heading.split('\n') if '\n' in heading else [heading]
        for line in lines:
            draw.text((x, y), line, font=heading_font, fill=self._hex_to_rgb("#FFFFFF") + (255,))
            y += 80

        if desc:
            y += 10
            draw.text((x, y), desc, font=body_font, fill=self._hex_to_rgb("#AAAAAA") + (255,))
            y += 50

        if cards:
            self._draw_cards(draw, x, y, cards)
        elif items:
            if layout == "list" or any('—' in it or '-' in it for it in items):
                self._draw_list(draw, x, y, items)
            else:
                self._draw_pills(draw, x, y, items)

    def _draw_cards(self, draw: ImageDraw.Draw, x: int, y: int, cards: List[Dict]):
        card_cfg = self.template.get("card", {})
        bg = self._hex_to_rgb(card_cfg.get("background", "#111111")) + (255,)
        border = self._hex_to_rgb(card_cfg.get("border_color", "#1A1A1A")) + (255,)
        accent = self._hex_to_rgb(card_cfg.get("accent_line.color", "#333333")) + (255,)
        pad = card_cfg.get("padding", 32)

        cols = 2 if len(cards) <= 4 else 3
        gap = 24
        card_w = (self.width - self.pad_x * 2 - gap * (cols - 1)) // cols

        start_x = x
        card_y = y

        for i, card in enumerate(cards):
            if i > 0 and i % cols == 0:
                start_x = x
                card_y += 160 + gap

            card_h = 140
            draw.rectangle([start_x, card_y, start_x + card_w, card_y + card_h], fill=bg)
            draw.rectangle([start_x, card_y, start_x + card_w, card_y + card_h], outline=border, width=1)
            draw.rectangle([start_x, card_y, start_x + 2, card_y + card_h], fill=accent)

            cx = start_x + pad
            cy = card_y + 20

            if card.get("subtitle"):
                draw.text((cx, cy), card["subtitle"], font=self.fonts.get("label"), fill=self._hex_to_rgb("#555555") + (255,))
                cy += 24

            draw.text((cx, cy), card["title"], font=self.fonts.get("heading"), fill=self._hex_to_rgb("#FFFFFF") + (255,))
            cy += 40

            if card.get("desc"):
                draw.text((cx, cy), card["desc"], font=self.fonts.get("body"), fill=self._hex_to_rgb("#888888") + (255,))

            start_x += card_w + gap

    def _draw_list(self, draw: ImageDraw.Draw, x: int, y: int, items: List[str]):
        list_cfg = self.template.get("list", {})
        font = self.fonts.get("body")
        strong_font = self.fonts.get("heading")
        bullet_color = self._hex_to_rgb(list_cfg.get("bullet_color", "#555555")) + (255,)
        text_color = self._hex_to_rgb(list_cfg.get("text_color", "#AAAAAA")) + (255,)
        strong_color = self._hex_to_rgb(list_cfg.get("strong_color", "#FFFFFF")) + (255,)
        line_h = int(list_cfg.get("line_height", 2.2) * 20)

        for item in items:
            if '—' in item:
                strong_part, desc_part = item.split('—', 1)
                strong_part = strong_part.strip()
                desc_part = desc_part.strip()

                draw.text((x, y), "▪", font=font, fill=bullet_color)
                sw, _ = self._text_size(draw, strong_part, strong_font)
                draw.text((x + 24, y), strong_part, font=strong_font, fill=strong_color)
                draw.text((x + 24 + sw + 10, y + 2), desc_part, font=font, fill=text_color)
            else:
                draw.text((x, y), "▪ " + item, font=font, fill=text_color)
            y += line_h

    def _draw_pills(self, draw: ImageDraw.Draw, x: int, y: int, items: List[str]):
        pill_cfg = self.template.get("pill", {})
        font = self.fonts.get("body")
        bg = self._hex_to_rgb(pill_cfg.get("background", "#151515")) + (255,)
        border = self._hex_to_rgb(pill_cfg.get("border_color", "#222222")) + (255,)
        text_c = self._hex_to_rgb(pill_cfg.get("text_color", "#AAAAAA")) + (255,)
        px = pill_cfg.get("padding_x", 18)
        py = pill_cfg.get("padding_y", 10)

        start_x = x

        for item in items:
            tw, th = self._text_size(draw, item, font)
            w = tw + px * 2
            h = th + py * 2

            if start_x + w > self.width - self.pad_x:
                start_x = x
                y += h + 12

            draw.rectangle([start_x, y, start_x + w, y + h], fill=bg, outline=border, width=1)
            draw.text((start_x + px, y + py), item, font=font, fill=text_c)

            start_x += w + 16

    def render_scene_video(self, scene: Dict, output_path: str, base_video_path: Optional[str] = None):
        duration = scene.get("duration", 4.0)
        total_frames = int(duration * DEFAULT_FPS)

        frames_dir = tempfile.mkdtemp(prefix="promo_frames_")
        base_frames_dir = None

        if base_video_path and os.path.exists(base_video_path):
            base_frames_dir = tempfile.mkdtemp(prefix="base_frames_")
            r = subprocess.run([
                'ffmpeg', '-y', '-i', base_video_path,
                '-vf', f'fps={DEFAULT_FPS},scale={self.width}:{self.height}',
                os.path.join(base_frames_dir, 'frame_%05d.png')
            ], capture_output=True, text=True, encoding='utf-8', errors='ignore')
            if r.returncode != 0:
                print(f"[警告] 提取引擎视频帧失败: {r.stderr[:300] if r.stderr else '未知'}")
                shutil.rmtree(base_frames_dir)
                base_frames_dir = None

        for i in range(total_frames):
            base_img = None
            if base_frames_dir:
                base_frame = os.path.join(base_frames_dir, f'frame_{i+1:05d}.png')
                if os.path.exists(base_frame):
                    base_img = Image.open(base_frame).convert("RGBA")

            frame = self.render_frame(scene, i, total_frames, base_img)
            frame = frame.convert("RGB")
            fp = os.path.join(frames_dir, f"frame_{i:05d}.png")
            frame.save(fp)

        pattern = os.path.join(frames_dir, "frame_%05d.png")
        cmd = [
            'ffmpeg', '-y', '-framerate', str(DEFAULT_FPS),
            '-i', pattern,
            '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
            '-preset', 'fast', '-crf', '18',
            '-an', output_path
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='ignore')
        if result.returncode != 0:
            print(f"[渲染错误] {result.stderr[-300:]}")

        shutil.rmtree(frames_dir)
        if base_frames_dir:
            shutil.rmtree(base_frames_dir)

        return output_path if os.path.exists(output_path) else None


# ============================================================
# 引擎录制器
# ============================================================
class EngineRecorder:
    def __init__(self, engine_path: str, output_dir: str):
        self.engine_path = engine_path
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)

    def record_scene(self, scene_name: str, duration: float, camera: str,
                     resolution: str = "1920x1080") -> Optional[str]:
        safe_name = scene_name.replace('/', '_').replace('\\', '_')
        output_path = os.path.join(self.output_dir, f"{safe_name}.mp4")

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
        result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='ignore')

        if result.returncode != 0:
            print(f"[警告] 录制失败，返回码: {result.returncode}")
            if result.stderr:
                print(f"        stderr: {result.stderr[:500]}")
            if result.stdout:
                print(f"        stdout: {result.stdout[:500]}")
            return None

        if not os.path.exists(output_path):
            print(f"[警告] 引擎未生成文件: {output_path}")
            print(f"        请确认引擎支持 --record / --output / --scene 参数")
            return None

        print(f"[完成] 素材已保存: {output_path}")
        return output_path


# ============================================================
# BGM 生成器
# ============================================================
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


# ============================================================
# 视频合成器
# ============================================================
class VideoComposer:
    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)

    def compose(self, scene_clips: List[str], bgm_path: str,
                fade_duration: float = 0.3, output_name: str = "trailer_final.mp4"):
        if not scene_clips:
            print("[错误] 没有素材可合成")
            return None

        processed = []
        for i, clip in enumerate(scene_clips):
            if clip is None or not os.path.exists(clip):
                continue
            out = os.path.join(self.output_dir, f"proc_{i:02d}.mp4")

            probe = subprocess.run([
                'ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                '-of', 'default=noprint_wrappers=1:nokey=1', clip
            ], capture_output=True, text=True, encoding='utf-8', errors='ignore')
            try:
                dur = float(probe.stdout.strip())
            except ValueError:
                dur = 4.0
            fade_out_start = max(0, dur - fade_duration)

            vf = f"fade=t=in:st=0:d={fade_duration},fade=t=out:st={fade_out_start}:d={fade_duration}"
            af = f"afade=t=in:st=0:d={fade_duration},afade=t=out:st={fade_out_start}:d={fade_duration}"

            cmd = ['ffmpeg', '-y', '-i', clip, '-vf', vf, '-af', af,
                   '-c:v', 'libx264', '-preset', 'fast', '-crf', '23',
                   '-c:a', 'aac', '-b:a', '128k', out]
            subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='ignore')
            processed.append(out)

        if not processed:
            return None

        concat_list = os.path.join(self.output_dir, "concat.txt")
        with open(concat_list, 'w', encoding='utf-8') as f:
            for p in processed:
                f.write(f"file '{p}'\n")

        concat_video = os.path.join(self.output_dir, "concat.mp4")
        subprocess.run(['ffmpeg', '-y', '-f', 'concat', '-safe', '0',
                        '-i', concat_list, '-c', 'copy', concat_video],
                       capture_output=True, text=True, encoding='utf-8', errors='ignore')

        final_path = os.path.join(self.output_dir, output_name)
        cmd = ['ffmpeg', '-y', '-i', concat_video, '-i', bgm_path,
               '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k',
               '-shortest', '-map', '0:v:0', '-map', '1:a:0',
               final_path]
        subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='ignore')

        print(f"[完成] 宣传片已生成: {final_path}")
        return final_path


# ============================================================
# 主流程
# ============================================================
def main():
    parser = argparse.ArgumentParser(description="PromoCreator - 游戏引擎宣传片自动生成器")
    parser.add_argument("config", help="Markdown 文案文件路径")
    parser.add_argument("--engine", default="GryceEngine.exe", help="引擎可执行文件路径")
    parser.add_argument("--template", default="dark", help="模板名称")
    parser.add_argument("--output-dir", default="./PromoCreator_output", help="输出目录")
    parser.add_argument("--skip-record", action="store_true", help="跳过录制，只合成")
    args = parser.parse_args()

    print(f"[1/6] 解析文案: {args.config}")
    md = SceneParser(args.config)
    print(f"      发现 {len(md.scenes)} 个场景")

    template_dir = Path(__file__).parent / "templates"
    template_path = template_dir / f"{args.template}.json"
    if not template_path.exists():
        print(f"[错误] 模板不存在: {template_path}")
        sys.exit(1)

    template = TemplateEngine(str(template_path))
    print(f"[2/6] 加载模板: {args.template}")

    fonts = FontManager(template)
    print(f"[3/6] 字体加载完成")

    clips_dir = os.path.join(args.output_dir, "clips")
    recorder = EngineRecorder(args.engine, clips_dir)
    renderer = SceneRenderer(template, fonts, clips_dir)

    scene_clips = []

    print(f"[4/6] 生成场景素材...")
    for i, scene in enumerate(md.scenes):
        scene_name = scene.get("record")
        has_record = scene_name is not None
        safe_name = scene_name.replace('/', '_').replace('\\', '_') if scene_name else f"scene_{i:02d}"

        engine_video = None
        if has_record and not args.skip_record:
            engine_video = recorder.record_scene(
                scene_name,
                scene["duration"],
                scene.get("camera", "static")
            )

        output_path = os.path.join(clips_dir, f"{safe_name}_final.mp4")
        rendered = renderer.render_scene_video(scene, output_path, engine_video)

        if rendered and os.path.exists(rendered):
            scene_clips.append(rendered)
        else:
            scene_clips.append(None)

    print(f"[5/6] 生成BGM...")
    bgm_path = os.path.join(args.output_dir, "bgm.wav")
    bgm_gen = BGMGenerator(bpm=template.get_bgm_config().get("bpm", 120))
    bgm_gen.generate(md.scenes, bgm_path)

    print(f"[6/6] 合成最终视频...")
    composer = VideoComposer(args.output_dir)
    final = composer.compose(
        scene_clips, bgm_path,
        fade_duration=template.get("fade_duration", 0.3)
    )

    if final:
        print(f"\n🎬 宣传片生成完成: {final}")
    else:
        print(f"\n❌ 生成失败")


if __name__ == "__main__":
    main()