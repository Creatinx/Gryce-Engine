# 模型导入与材质自动识别

把任意模型丢进任意场景都应该"看起来对"：贴图自己找齐、参数按文件里写的来、
金属就是金属、布就是布。本文记录这条链路的规则和排查手法。

代码位置：

| 文件 | 职责 |
| --- | --- |
| `engine/assets/assimp_importer.cpp` | FBX / glTF / DAE / PLY / STL / 3DS / Blend 导入（Assimp） |
| `engine/assets/obj_loader.cpp` | OBJ + MTL 内置加载器（快、UV 朝向可控、支持多材质） |
| `engine/assets/material_rules.h/.cpp` | **格式无关**的 PBR 推导规则（两种导入器共用） |
| `engine/assets/texture_path.h` | 贴图路径解析（内嵌 / 绝对 / 相对 / 大小写） |
| `engine/components/mesh_renderer.cpp` | 把文件材质合并进组件材质（显式设置优先） |
| `engine/scene/multi_material_assembly.cpp` | 多子网格实体拆成父 + 子实体 |

---

## 1. 贴图：先找到，再读对

一次导入会为每个子网格解析 6 个槽位：`albedo / normal / roughness / metallic / ao / emissive`。
每个槽位按优先级取第一张**真的能打开**的图：

| 槽位 | 候选 `aiTextureType` |
| --- | --- |
| albedo | `BASE_COLOR`（glTF）→ `DIFFUSE` |
| normal | `NORMAL_CAMERA` → `NORMALS` |
| roughness | `DIFFUSE_ROUGHNESS` |
| metallic | `METALNESS` |
| ao | `AMBIENT_OCCLUSION` → `LIGHTMAP` |
| emissive | `EMISSIVE` → `EMISSION_COLOR` |

路径解析顺序（`texture_path::resolve`）：

1. 绝对路径：文件真的存在才用（FBX 里常残留导出机的 `D:\art\...`，换机必然失效）
2. 相对模型目录（或 MTL 所在目录）
3. 按文件名在模型目录下找
4. 在 `textures/`、`Textures/`、`maps/`、`Maps/`、`texture/`、`../textures/`、`../maps/` 里找
5. 以上全部大小写不敏感扫描（macOS/Linux 导出的 `Wood.JPG` 对不上 NTFS 的 `wood.jpg`）

内嵌贴图（`"*N"` 引用 `aiScene::mTextures`）会被提取落盘到
`%TEMP%/gryce-embedded-textures/embedded_<crc32>.<ext>`。文件名带**内容哈希**：
不同模型的内嵌贴图都从 `*0` 开始编号，只用索引命名会让后导入的模型拿到前一个模型的图。

**打包贴图自动拆通道**：glTF 的 `metallicRoughness` 与常见 ORM 图把三张灰度图打包在一张
（R=AO、G=roughness、B=metallic）。渲染侧三个槽位各自采样 `.r`，共用一张图会导致三者
全读 R 通道（"文件里写了粗糙度，模型却要么全金属要么全粗糙"）。导入时若 roughness 与
metallic 指向同一文件，会拆成单通道 PNG 存到 `%TEMP%/gryce-derived-maps/` 并改引用。

---

## 2. 参数：从文件里读，读不到才推

规则集中在 `material_rules.cpp::resolve_surface()`，纯函数，`ctest -R material` 覆盖。

| 参数 | 优先级 |
| --- | --- |
| roughness | `$mat.roughnessFactor`（FBX Physical / glTF）→ `Shininess` 换算 → 有基色贴图时 0.8 / 纯色 0.5 |
| metallic | `$mat.metallicFactor` → spec-gloss 反推（见下）→ 0 |
| base color | `$clr.diffuse`；判定为金属时改用镜面颜色 |
| opacity | `$mat.opacity` → `1 - transparencyFactor` → 1（<0.999 触发混合 + 双面） |
| two-sided | 文件声明 / 半透明材质一律双面 |
| emissive | `$clr.emissive`；只给了自发光贴图时按白色（否则乘 0 看不见） |

`Shininess` 换算用的是 GGX 一致式：`r = (2/(n+2))^(1/4)`。
旧的 `sqrt(2/(n+2))` 把 α 当 r 用，n=1000 时给出 0.045（正确 0.21）——
整模型比文件写的亮得多，泛出金属般的镜面感。

**spec-gloss 反推金属度**的判据（只在没有 metallic 字段时用）：

```
没有基色贴图 且 漫反射亮度 < 0.06 且 镜面亮度 > 0.20  →  metallic = clamp(镜面亮度), albedo = 镜面颜色
```

- 有基色贴图时 `Kd` 只是"贴图乘子"（3ds Max 默认 0.5 灰），拿它当漫反射强度会把整个
  摄影测量模型判成金属 —— 这是历史事故，规则里显式禁掉。
- FBX 的 phong 材质即使没写 specular，assimp 也会填 0.9 灰；只有 `$raw.Specular` 非零
  才算"作者写了镜面反射"。否则老 FBX 会被整体误判成金属。
- OBJ 由我们自己的 MTL 解析器读，只有文件里真有 `Ks` 才算写了镜面。

## 3. 表面类型：决定怎么参与光照

`SurfaceType` 由上面几个值推出来，日志里会打出 `<材质名> -> Metal/Dielectric/Transparent/Emissive`：

| 类型 | 判定 | 光照行为 |
| --- | --- | --- |
| Transparent | opacity < 0.999 | alpha 混合 pass + 双面 |
| Emissive | 自发光亮度 > 0.05 | 叠加 emissive，不依赖光源 |
| Metal | metallic > 0.5 | `F0 = albedo`，几乎无漫反射，靠 IBL / SSR 出内容 |
| Dielectric | 其余 | `F0 ≈ 0.04`，以漫反射为主 |

---

## 4. 色彩空间：颜色图走 sRGB，数据图必须线性

渲染管线最后一步是 `pow(1/2.2)` 编码输出，所以颜色贴图必须在采样时解码到线性空间。
不解码时 sRGB 0.5 会被当成线性 0.5 参与光照（真实值应是 0.21）：漫反射偏亮、饱和度
偏低，整片表面看起来像"白塑料 / 金属"。

- `Material::upload_to_gpu()` 里 **albedo / emissive** 用 `set_srgb(true)`
- normal / roughness / metallic / ao 保持线性（开 sRGB 等于给数值做 gamma）
- 三后端各自落到 `GL_SRGB8(_ALPHA8)` / `VK_FORMAT_*_SRGB` / `DXGI_FORMAT_*_SRGB`；
  mip 生成与各向异性过滤也在线性空间完成
- 已知限制：块压缩格式（BC/ETC/ASTC）目前没有 `*_SRGB` 变体，压缩过的颜色贴图
  仍按线性上传（会偏亮一档），日志里会告警

---

## 5. 覆盖规则：场景里写过的一律优先

`Material` 有 `albedo_overridden / roughness_overridden / metallic_overridden` 三个标志，
由场景 JSON（写了该键）、`set_*()`、预设、材质 API 置位。合并模型材质时**只看标志**
（不是"值是否等于默认值"，否则没法把 metallic 显式设成 0）。

多子网格实体的子实体通过 `copy_settings_from` 继承父实体的材质参数，所以在一份
`.gesc` 里给父实体写 `roughness` / `metallic` 就能一次管住整模型的所有子网格。

---

## 6. 排查手法

导入时会为每个材质打一行日志：

```
AssimpImporter: material 'Stone' -> Dielectric (roughness 0.88, metallic 0.01, opacity 1,
                maps: albedo=1, normal=0, rough=0, metal=0, ao=0, emissive=0)
ObjLoader: material 'Gold' -> Metal (roughness 0.223, metallic 0.788, ...)
```

没有对应的图时会打 `references missing texture '...' (searched '...')`，可以直接看出
是路径问题还是文件真的不在。

再往下就要看文件里到底存了什么：直接用 Assimp 打开模型，遍历 `aiMaterial::mProperties`
把 `$mat.*` / `$raw.*` 原始值和所有贴图槽位打出来（十几行代码的临时探针即可），
确认"文件里到底有没有这个字段"——比在渲染侧猜快得多。

渲染侧的常见误判：

| 现象 | 先查 |
| --- | --- |
| 整模型像金属 | `metallic` 日志值；是否被 spec-gloss 反推误判；`ssr_max_roughness` 是否把像素放进了 SSR；下方「6.1」的 IBL 镜面口径 |
| 贴图在但看着糊/发白 | 是不是压缩贴图（sRGB 未生效）、mip 链是否建全 |
| 材质参数改了没反应 | 场景 JSON 是否显式写死了该字段（`*_overridden`）；有基色贴图时 `albedo_color` 是**三目忽略**不是相乘 |
| 半透明模型背面没有 | `two_sided` / 是否走了混合 pass |

### 6.1 "像金属"不等于材质是金属

先分清三种完全不同的成因，否则会一直在材质参数里打转：

1. **真的 metallic > 0**。看导入日志/场景 JSON，或 `--debug 4` 直接出 metallic 视图。
2. **IBL 镜面口径错**。这是最容易走眼的一类：物理上 `F0=0.04` 的电介质本来就有一点点反射，
   但口径一错就会变成"整片表面都镀了一层"。两个必须守住的点：
   - `spec = prefiltered * (F0 * A + B)`。split-sum 的 LUT 已经把 Fresnel 积分掉了，**不**能
     再用 `fresnel(NdotV)` 去乘 A —— 掠射角 F→1，粗糙面的权重会从 `0.04*A≈0.02` 顶到 `A≈0.9`，
     石墙、水泥、砖在斜视角全被算成镜面。
   - BRDF LUT 的积分里，被剔除的样本（`NdotL<=0`）必须贡献 0，不能当成满额镜面累加。
     错了的表现是粗糙面的 A 直接翻倍（0.52 → 0.95）。
   两者的共同症状：**画面发灰、对比度腰斩、颜色发白，但不均匀——掠射角/边缘更亮**。
   快速判据：`--no-env` 渲一张，如果对比度（像素 std）明显回升就是这个方向。
3. **摄影测量资产的二次照明**。扫描模型的 albedo 贴图本身就是一张带阳光和阴影的照片，
   实时太阳 + IBL 再叠一遍等于曝光两次 → 高光糊成一片、饱和度掉光。这类资产要把
   `ibl_intensity` 调到 0.5~0.7、太阳别开太大；同时 `ssr_max_roughness` 要低于材质自身的
   roughness（石材 0.86 跑 SSR 只会给整面墙涂一层天空色）。

判据汇总（同一机位全图统计，`--no-debug-ui`）：修好后饱和度/对比度应同时上升；`--no-env`
不应再有肉眼可见的"回魂"。
