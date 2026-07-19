# 生成最小 skinned glTF 测试资产（2 骨骼 + 3 顶点 + 1 段位移动画）
# 用法：py tools/gen_skinned_fixture.py
import struct, base64, json, os

buf = bytearray()
views = []
accessors = []

def align4():
    while len(buf) % 4 != 0:
        buf.append(0)

def add_view(data, target=None):
    align4()
    off = len(buf)
    buf.extend(data)
    v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
    if target:
        v["target"] = target
    views.append(v)
    return len(views) - 1

def add_accessor(view, comp_type, count, acc_type, minv=None, maxv=None):
    a = {"bufferView": view, "byteOffset": 0, "componentType": comp_type,
         "count": count, "type": acc_type}
    if minv is not None:
        a["min"] = minv
    if maxv is not None:
        a["max"] = maxv
    accessors.append(a)
    return len(accessors) - 1

FLOAT, USHORT = 5126, 5123

# positions：三角形 3 顶点
pos = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
a_pos = add_accessor(add_view(b"".join(struct.pack("<3f", *p) for p in pos), 34962),
                     FLOAT, 3, "VEC3", [0, 0, 0], [1, 1, 0])

# UV（供 CalcTangentSpace）
uvs = [(0, 0), (1, 0), (0, 1)]
a_uv = add_accessor(add_view(b"".join(struct.pack("<2f", *t) for t in uvs), 34962),
                    FLOAT, 3, "VEC2")

# JOINTS_0 (ushort)：v0→joint0，v1→joint1，v2→joint0+joint1
joints = [(0, 0, 0, 0), (1, 0, 0, 0), (0, 1, 0, 0)]
a_j = add_accessor(add_view(b"".join(struct.pack("<4H", *j) for j in joints), 34962),
                   USHORT, 3, "VEC4")

# WEIGHTS_0 (float)：v0 全 joint0，v1 全 joint1，v2 各半
weights = [(1, 0, 0, 0), (1, 0, 0, 0), (0.5, 0.5, 0, 0)]
a_w = add_accessor(add_view(b"".join(struct.pack("<4f", *w) for w in weights), 34962),
                   FLOAT, 3, "VEC4")

# indices
a_i = add_accessor(add_view(struct.pack("<3H", 0, 1, 2), 34963), USHORT, 3, "SCALAR")

# inverse bind matrices（列主序与 glTF 一致）：joint0 identity，joint1 = T(0,-1,0)
ibm_identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
ibm_joint1 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]
a_ibm = add_accessor(add_view(struct.pack("<16f", *ibm_identity) + struct.pack("<16f", *ibm_joint1)),
                     FLOAT, 2, "MAT4")

# 动画：joint1 translation (0,1,0) → (0,3,0)，1 秒
a_in = add_accessor(add_view(struct.pack("<2f", 0.0, 1.0)), FLOAT, 2, "SCALAR", [0.0], [1.0])
a_out = add_accessor(add_view(struct.pack("<3f", 0, 1, 0) + struct.pack("<3f", 0, 3, 0)),
                     FLOAT, 2, "VEC3")

uri = "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode()

gltf = {
    "asset": {"version": "2.0", "generator": "gryce-test-fixture"},
    "scene": 0,
    "scenes": [{"nodes": [0, 2]}],
    "nodes": [
        {"name": "joint0", "children": [1]},
        {"name": "joint1", "translation": [0, 1, 0]},
        {"name": "meshNode", "mesh": 0, "skin": 0},
    ],
    "meshes": [{
        "primitives": [{
            "attributes": {"POSITION": a_pos, "TEXCOORD_0": a_uv,
                           "JOINTS_0": a_j, "WEIGHTS_0": a_w},
            "indices": a_i,
        }]
    }],
    "skins": [{"joints": [0, 1], "inverseBindMatrices": a_ibm}],
    "animations": [{
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}],
        "samplers": [{"input": a_in, "interpolation": "LINEAR", "output": a_out}],
    }],
    "buffers": [{"byteLength": len(buf), "uri": uri}],
    "bufferViews": views,
    "accessors": accessors,
}

out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures", "models")
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.join(out_dir, "skinned_triangle.gltf")
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(gltf, f)
print("written:", out_path, "buffer:", len(buf), "bytes")
