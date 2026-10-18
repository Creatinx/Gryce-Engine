# Model Import and Automatic Material Recognition

Dropping any model into any scene should "look right": textures found on their own, parameters taken from the file, metal looking like metal and cloth like cloth. This document records the rules of this pipeline and the troubleshooting techniques.

Code locations:

| File | Responsibility |
| --- | --- |
| `engine/assets/assimp_importer.cpp` | FBX / glTF / DAE / PLY / STL / 3DS / Blend import (Assimp) |
| `engine/assets/obj_loader.cpp` | Built-in OBJ + MTL loader (fast, controllable UV orientation, multi-material) |
| `engine/assets/material_rules.h/.cpp` | **Format-agnostic** PBR derivation rules (shared by both importers) |
| `engine/assets/texture_path.h` | Texture path resolution (embedded / absolute / relative / case-insensitive) |
| `engine/components/mesh_renderer.cpp` | Merges file materials into component materials (explicit settings win) |
| `engine/scene/multi_material_assembly.cpp` | Splits multi-submesh entities into parent + child entities |

---

## 1. Textures: Find Them First, Read Them Correctly

One import resolves six slots per submesh: `albedo / normal / roughness / metallic / ao / emissive`. Each slot takes the first candidate that can actually be opened:

| Slot | Candidate `aiTextureType` |
| --- | --- |
| albedo | `BASE_COLOR` (glTF) -> `DIFFUSE` |
| normal | `NORMAL_CAMERA` -> `NORMALS` |
| roughness | `DIFFUSE_ROUGHNESS` |
| metallic | `METALNESS` |
| ao | `AMBIENT_OCCLUSION` -> `LIGHTMAP` |
| emissive | `EMISSIVE` -> `EMISSION_COLOR` |

Path resolution order (`texture_path::resolve`):

1. Absolute paths: used only if the file really exists (FBX files often retain the export machine's `D:\art\...`, which always breaks on another machine)
2. Relative to the model directory (or the MTL directory)
3. Search by file name under the model directory
4. Search in `textures/`, `Textures/`, `maps/`, `Maps/`, `texture/`, `../textures/`, `../maps/`
5. All of the above are case-insensitive scans (`Wood.JPG` exported from macOS/Linux will not match NTFS's `wood.jpg`)

Embedded textures (references `"*N"` into `aiScene::mTextures`) are extracted to
`%TEMP%/gryce-embedded-textures/embedded_<crc32>.<ext>`. File names carry a **content hash**:
embedded textures of different models all start numbering at `*0`, so index-only naming would give a later-imported model the previous model's texture.

**Packed textures are split automatically**: glTF's `metallicRoughness` and common ORM maps pack three grayscale maps into one (R=AO, G=roughness, B=metallic). If the three render-side slots each sample `.r` from the same image, all three read the R channel ("the file specifies roughness, yet the model is either all metal or all rough"). At import time, if roughness and metallic point to the same file, it is split into single-channel PNGs stored in `%TEMP%/gryce-derived-maps/` and the references are updated.

---

## 2. Parameters: Read From the File First, Infer Only When Absent

Rules are centralized in `material_rules.cpp::resolve_surface()`, a pure function covered by `ctest -R material`.

| Parameter | Priority |
| --- | --- |
| roughness | `$mat.roughnessFactor` (FBX Physical / glTF) -> `Shininess` conversion -> 0.8 with a base color map / 0.5 for flat color |
| metallic | `$mat.metallicFactor` -> spec-gloss inversion (see below) -> 0 |
| base color | `$clr.diffuse`; switches to the specular color when classified as metal |
| opacity | `$mat.opacity` -> `1 - transparencyFactor` -> 1 (<0.999 triggers blending + two-sided) |
| two-sided | declared in file / always two-sided for transparent materials |
| emissive | `$clr.emissive`; white when only an emissive map is given (otherwise multiplied by 0 it is invisible) |

The `Shininess` conversion uses the GGX-consistent formula: `r = (2/(n+2))^(1/4)`.
The older `sqrt(2/(n+2))` treated alpha as r, giving 0.045 at n=1000 (correct: 0.21) --
the whole model rendered much brighter than the file specifies, with a metallic specular sheen.

**Spec-gloss inversion of metallicness** (only used when no metallic field exists):

```
No base color map AND diffuse luminance < 0.06 AND specular luminance > 0.20  ->  metallic = clamp(specular luminance), albedo = specular color
```

- With a base color map, `Kd` is only a "texture multiplier" (3ds Max defaults to 0.5 gray); treating it as diffuse intensity would classify an entire photogrammetry model as metal -- a historical incident, explicitly forbidden by the rules.
- FBX phong materials get a 0.9 gray specular from assimp even without an authored specular; only a non-zero `$raw.Specular` counts as "the author wrote specular". Otherwise old FBX files would be misclassified as metal wholesale.
- OBJ is read by our own MTL parser; only a real `Ks` in the file counts as authored specular.

## 3. Surface Types: How They Participate in Lighting

`SurfaceType` is derived from the values above; the log prints `<material name> -> Metal/Dielectric/Transparent/Emissive`:

| Type | Condition | Lighting behavior |
| --- | --- | --- |
| Transparent | opacity < 0.999 | alpha blend pass + two-sided |
| Emissive | emissive luminance > 0.05 | adds emissive, does not depend on lights |
| Metal | metallic > 0.5 | `F0 = albedo`, almost no diffuse, content comes from IBL / SSR |
| Dielectric | everything else | `F0 ~ 0.04`, primarily diffuse |

---

## 4. Color Space: Color Maps sRGB, Data Maps Linear

The pipeline's final step is `pow(1/2.2)` output encoding, so color textures must be decoded to linear at sample time. Without decoding, sRGB 0.5 is treated as linear 0.5 in lighting (the true value is 0.21): diffuse is too bright, saturation too low, and whole surfaces look like "white plastic / metal".

- In `Material::upload_to_gpu()`, **albedo / emissive** use `set_srgb(true)`
- normal / roughness / metallic / ao stay linear (enabling sRGB on them gammas the values)
- The three backends land on `GL_SRGB8(_ALPHA8)` / `VK_FORMAT_*_SRGB` / `DXGI_FORMAT_*_SRGB`; mip generation and anisotropic filtering also happen in linear space
- Known limitation: block-compressed formats (BC/ETC/ASTC) currently have no `*_SRGB` variants, so compressed color textures still upload as linear (one stop too bright), with a log warning

---

## 5. Override Rules: Anything Written in the Scene Wins

`Material` has `albedo_overridden / roughness_overridden / metallic_overridden` flags, set by the scene JSON (key present), `set_*()`, presets, or the material API. When merging model materials, **only the flags are consulted** (not "whether the value equals the default", otherwise metallic could not be explicitly set to 0).

Child entities of multi-submesh entities inherit the parent's material parameters via `copy_settings_from`, so writing `roughness` / `metallic` on the parent entity in one `.gesc` controls all submeshes of the model at once.

---

## 6. Troubleshooting

Each material logs one line at import:

```
AssimpImporter: material 'Stone' -> Dielectric (roughness 0.88, metallic 0.01, opacity 1,
                maps: albedo=1, normal=0, rough=0, metal=0, ao=0, emissive=0)
ObjLoader: material 'Gold' -> Metal (roughness 0.223, metallic 0.788, ...)
```

When a referenced image is missing, the log prints `references missing texture '...' (searched '...')`, which shows directly whether it is a path problem or the file is really absent.

Below that, inspect what the file actually stores: open the model with Assimp directly, iterate `aiMaterial::mProperties`, and print the raw `$mat.*` / `$raw.*` values plus all texture slots (a temporary probe of a dozen lines suffices) to confirm "does this field exist in the file at all" -- much faster than guessing on the render side.

Common misjudgments on the render side:

| Symptom | Check first |
| --- | --- |
| Whole model looks like metal | the `metallic` log value; whether spec-gloss inversion misfired; whether `ssr_max_roughness` put the pixels into SSR; the IBL specular convention in section 6.1 below |
| Texture present but blurry / washed out | Is it a compressed texture (sRGB not applied)? Is the mip chain complete? |
| Material parameter changes have no effect | Does the scene JSON explicitly pin the field (`*_overridden`)? With a base color map, `albedo_color` is a **ternary override**, not a multiply |
| Transparent model has no back faces | `two_sided` / whether it went through the blend pass |

### 6.1 "Looks Like Metal" Does Not Mean the Material Is Metal

Distinguish three completely different causes first, otherwise you will keep circling the material parameters:

1. **Really metallic > 0**. Check the import log / scene JSON, or use `--debug 4` for a direct metallic view.
2. **Wrong IBL specular convention**. This is the easiest to miss: physically, a dielectric with `F0=0.04` already has a little reflection, but a wrong convention turns it into "the whole surface is plated". Two points must hold:
   - `spec = prefiltered * (F0 * A + B)`. The split-sum LUT has already integrated Fresnel, so A **must not** be multiplied by `fresnel(NdotV)` again -- at grazing angles F->1, and the rough-surface weight goes from `0.04*A~0.02` up to `A~0.9`, making stone walls, concrete, and brick all specular at oblique angles.
   - In the BRDF LUT integration, rejected samples (`NdotL<=0`) must contribute 0, not be accumulated as full specular. When wrong, rough surfaces' A doubles outright (0.52 -> 0.95).
   Shared symptom of both: **the picture turns gray, contrast is cut in half, colors wash out, but unevenly -- grazing angles / edges are brighter**. Quick test: render one frame with `--no-env`; if contrast (pixel std) clearly recovers, this is the direction.
3. **Double lighting of photogrammetry assets**. A scanned model's albedo texture is itself a photo with sunlight and shadows baked in; stacking real-time sun + IBL on top exposes it twice -> highlights smear into a blob and saturation drops. For such assets, set `ibl_intensity` to 0.5~0.7, keep the sun modest, and keep `ssr_max_roughness` below the material's own roughness (stone at 0.86 running SSR just coats the whole wall in sky color).

Summary of criteria (full-frame statistics at the same camera position, `--no-debug-ui`): after a fix, saturation and contrast should both rise; `--no-env` should no longer produce a visible "resurrection".
