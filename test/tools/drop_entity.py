#!/usr/bin/env python3
"""从 demo 场景里删掉一个实体（探针用）：python drop_entity.py <gesc> <name>

比字符串裁剪安全：按 JSON 解析后按 name 过滤，避免把相邻实体一起截掉
（实测手写 substr 裁剪会把实体边界搞错，留下半截 JSON，导致场景静默加载失败）。
"""
import json
import sys

path, name = sys.argv[1], sys.argv[2]
with open(path, encoding="utf-8") as f:
    data = json.load(f)
before = len(data["entities"])
data["entities"] = [e for e in data["entities"] if e.get("name") != name]
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, ensure_ascii=False)
print(f"{path}: entities {before} -> {len(data['entities'])}")
