// =============================================================================
// d3d12_vulkanize — 把 GL(330/430 core) 着色器源码改写为 Vulkan-GLSL
//
// 背景（M4 的关键发现）：引擎的着色器是**为桌面 OpenGL 写的 330/430 core**——
// 老式的 `uniform`、无 `layout(location=)` 的 `in/out`、裸 `uniform sampler2D`。
// 这与 Vulkan 专用的 `vulkan_*.vert/frag`（450 + uniform block + push constant）
// 是两套完全独立的文件；只有 `engine/render/shaders/forward_clustered/*` 下的
// 那批（scene_pbr / fog / decal / ...）是 GL 与 Vulkan 共用的，也正是 D3D12
// 必须能编译的那批。
//
// D3D12 的着色器链是 GLSL→SPIR-V→HLSL，而 **shaderc 只能生成 Vulkan 语义的
// SPIR-V**，它对 GL 方言零容忍：
//   * "'location' : SPIR-V requires location for user input/output"
//   * "'non-opaque uniforms outside a block' : not allowed when using GLSL for Vulkan"
//   * "'binding' : sampler/texture/image requires layout(binding=X)"
//
// 硬改 60+ 个 GL 着色器文件风险太高（会动到正在工作的 GL/Vulkan 路径，违反
// "改一处必须双后端验证" 的纪律）。所以这里做**源码级自动改写**：
// 保持单一事实来源（原 .vert/.frag 不动），仅在送进 D3D12 链之前变换。
//
// 改写规则（纯文本，逐行确定性）：
//   1. `#version 3x0 core`        → `#version 450`
//   2. 所有 `in` / `out` 显式补 `layout(location = N)`
//      - 顶点输入：优先复用显式 location，否则按变量名猜语义（aPos→0 …）
//      - varying（顶点 out / 片元 in）：**两侧各自独立从 0 顺序编号**，
//        前提是两 stage 的声明顺序一致（本工程满足）。
//   3. `uniform <type> <name>;`  → 全部收拢进
//      `layout(set = 0, binding = 0, std140) uniform Globals { ... };`
//   4. `uniform sampler* <name>;` → `layout(set = 0, binding = N) uniform sampler* ...`
//      （binding 从 1 起顺排，0 留给 Globals）
//   5. 同时产出 std140 布局元数据（offset/size/HLSL 类型），供 D3D12 侧
//      建 root signature 的 CBV 与写 uniform 时定位。
//   6. `gl_InstanceID` → `gl_InstanceIndex`（Vulkan 无前者）。
//   7. 文件作用域的 `const int NAME = <字面量>;` 提到 `#version` 之后
//      （uniform 块里可能引用它当数组维度，如 skinned_pbr 的 MAX_BONES）。
//   8. `uniform T x = <常量表达式>;` 的初始化式记进 initializer 字段。
//
// 注意：`gl_FragColor` 不需要处理 —— 本工程 GL 片元着色器写的是自定义
// `out vec4 FragColor;`，只缺 location。（vulkan_*.frag 那批才用 gl_FragColor，
// 但它们是 Vulkan 专用，D3D12 走不到。）
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::render::d3d12 {

// 本次改写的着色阶段（决定 in/out 的位置分配策略）。
enum class GlslVariant {
    Vertex,
    Fragment,
    Compute,
};

// 改写结果。
struct VulkanizedShader {
    std::string source;               // 改写后的 Vulkan-GLSL 源码
    bool ok = false;                  // 是否完成改写
    std::string error;                // 失败原因（当前仅在空输入时使用）

    // Globals 块里的字段（按声明序，offset 为 std140 字节偏移）。
    struct UniformBlockField {
        std::string type;             // HLSL 类型名（float4x4 / float3 / ...）
        std::string std140_type;      // GLSL 类型名（mat4 / vec3 / ...）
        std::string name;             // 变量名
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t array_count = 0;     // 数组元素数；表达式(如 MAX_LIGHTS)时为 0
        std::string array_suffix;     // 原样数组后缀（"[4]" / "[MAX_LIGHTS]"），空=非数组
        // ★ GLSL 的 `uniform float uExposure = 1.0;` 有编译期默认值。搬进 uniform
        //   块后成员不允许带初始化式，所以这里把表达式原样留下，由 D3D12 侧在
        //   着色器构造时求值写进 CBV 影子内存的**默认快照**。
        //   不保留会让 tonemap 的 uExposure/uGamma/uSaturation 等变成 0
        //   （GL 下是 1.0）→ 画面全黑 / 全灰。空串表示没有初始化式。
        std::string initializer;
    };
    std::vector<UniformBlockField> uniform_fields;
    uint32_t uniform_block_size = 0;   // Globals 块总字节数（16 对齐）

    std::vector<std::string> sampler_names;          // 纹理变量名（按 binding 顺序）
    std::vector<std::string> vertex_input_semantics;// 顶点输入语义名（POSITION/NORMAL/...）
    std::vector<uint32_t> vertex_input_locations;    // 对应 location
};

// 顶点语义查表（与 d3d12_mesh::location_to_semantic 保持一致）。
// 返回 nullptr 表示该 location 没有语义。
const char* vertex_semantic_for_location(uint32_t location);
// 同语义名重复时使用的语义索引（TEXCOORD0 / TEXCOORD1 / TEXCOORD2）。
const char* vertex_semantic_index_for_location(uint32_t location);

// ---------------------------------------------------------------------------
// 求值 GLSL 常量表达式（uniform 初始化式）并写进 std140 字节缓冲
// ---------------------------------------------------------------------------
// 支持的形态（覆盖本工程全部实际用法）：
//   * 标量字面量        `1.0` / `-1.0` / `.35` / `1` / `true` / `false`
//   * 向量构造          `vec4(0.0)`（广播）/ `vec2(1280.0, 720.0)`
//   * 矩阵构造          `mat4(1.0)`（对角矩阵）
// 不支持则返回 false（调用方保持零值并记录一次告警）。
// `written` 返回写入的实际字节数（vec3 只写 12 字节，末尾 padding 不动）。
bool eval_glsl_const(const std::string& expr, const std::string& glsl_type,
                     uint8_t* dst, uint32_t max_bytes, uint32_t& written);

// 把单个 GL 着色器源码改写为 Vulkan-GLSL。
VulkanizedShader vulkanize_glsl(const std::string& source, GlslVariant variant);

} // namespace gryce_engine::render::d3d12
