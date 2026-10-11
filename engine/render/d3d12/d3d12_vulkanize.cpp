// d3d12_vulkanize.cpp — 见头文件注释。
//
// 设计约束（务必保持）：
//   * 纯文本、逐行、确定性变换，**不改原 .vert/.frag**（GL/Vulkan 主路径零影响）。
//   * 不依赖任何第三方 GLSL 解析器，避免把 D3D12 的可选依赖扩散到整个构建。
//   * 结果只需满足"能被 shaderc 编成 Vulkan SPIR-V"，故一切按 Vulkan 语义来。

#include "d3d12_vulkanize.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gryce_engine::render::d3d12 {

// ★ SSBO（Vulkan GLSL 的 `layout(std430, binding = N)` 块）在 D3D12 侧的
//   寄存器基址。采样器固定在 t1..tN（pbr/instanced 系列实测 25 张），取 64 是
//   留足两倍以上余量：一条 SRV 表内多占的槽位只是描述符，代价远小于"区间重叠
//   导致的设备移除"。详见重建循环里的说明。
constexpr int kStorageBindingBase = 64;

namespace {

// GLSL 内置名（不参与 location 分配、也不进 uniform 块）。
bool is_glsl_builtin_name(const std::string& n) {
    return n.size() >= 3 && n[0] == 'g' && n[1] == 'l' && n[2] == '_';
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

// 取行首的可见内容（跳过空白）。
std::string ltrim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    return s.substr(b);
}

std::string leading_indent(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == ' ' || c == '\t') out.push_back(c);
        else break;
    }
    return out;
}

// 去掉行内 // 注释（本工程着色器里 // 不会出现在字符串字面量中）。
std::string strip_line_comment(const std::string& s) {
    bool in_str = false;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (c == '"') in_str = !in_str;
        if (!in_str && c == '/' && s[i + 1] == '/') return s.substr(0, i);
    }
    return s;
}

// 去掉块注释（跨行），保留换行以维持行号。
std::string strip_block_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_block = false;
    for (size_t i = 0; i < src.size(); ++i) {
        if (!in_block && i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            in_block = true;
            ++i;
            continue;
        }
        if (in_block && i + 1 < src.size() && src[i] == '*' && src[i + 1] == '/') {
            in_block = false;
            ++i;
            continue;
        }
        if (in_block) {
            if (src[i] == '\n') out.push_back('\n');
            continue;
        }
        out.push_back(src[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// GLSL 类型 → std140 布局 + HLSL 类型
// ---------------------------------------------------------------------------

struct TypeInfo {
    uint32_t size = 0;            // 成员字节大小（不含数组）
    uint32_t align = 0;           // std140 对齐
    const char* hlsl = "float";   // HLSL 类型名
};

bool lookup_type(const std::string& t, TypeInfo& out) {
    static const std::map<std::string, TypeInfo> kTable = {
        {"float",  {4,  4,  "float"}},
        {"int",    {4,  4,  "int"}},
        {"uint",   {4,  4,  "uint"}},
        {"bool",   {4,  4,  "bool"}},
        {"vec2",   {8,  8,  "float2"}},
        {"vec3",   {12, 16, "float3"}},
        {"vec4",   {16, 16, "float4"}},
        {"ivec2",  {8,  8,  "int2"}},
        {"ivec3",  {12, 16, "int3"}},
        {"ivec4",  {16, 16, "int4"}},
        {"uvec2",  {8,  8,  "uint2"}},
        {"uvec3",  {12, 16, "uint3"}},
        {"uvec4",  {16, 16, "uint4"}},
        {"mat2",   {16, 8,  "float2x2"}},
        {"mat3",   {48, 16, "float3x3"}},
        {"mat4",   {64, 16, "float4x4"}},
        {"mat3x3", {48, 16, "float3x3"}},
        {"mat4x4", {64, 16, "float4x4"}},
    };
    auto it = kTable.find(t);
    if (it == kTable.end()) return false;
    out = it->second;
    return true;
}

uint32_t align_up(uint32_t v, uint32_t a) {
    return a ? (v + a - 1) / a * a : v;
}

// ---------------------------------------------------------------------------
// 声明解析
// ---------------------------------------------------------------------------

enum class Direction { None, In, Out };

struct Decl {
    Direction dir = Direction::None;
    bool is_uniform = false;
    bool is_sampler = false;
    bool flat = false;
    std::string type;          // 规范化（去空白）类型名
    std::string name;
    std::string array_suffix;  // "[N]" 原样，无则空
    std::string array_expr;    // 方括号内的原文（"4" / "MAX_LIGHTS"）
    int array_count = 0;
    int location = -1;
    std::string initializer;   // 初始化式原文（`=` 与 `;` 之间），无则空
    size_t line_index = 0;
};

void parse_layout_prefix(const std::string& s, size_t& pos, Decl& d, bool& has_binding) {
    while (pos < s.size() && starts_with(s.substr(pos, 6), "layout")) {
        size_t open = s.find('(', pos);
        if (open == std::string::npos) break;
        int depth = 0;
        size_t i = open;
        for (; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') { --depth; if (depth == 0) break; }
        }
        if (i >= s.size()) break;
        const std::string body = s.substr(open + 1, i - open - 1);
        size_t lp = body.find("location");
        if (lp != std::string::npos) {
            size_t eq = body.find('=', lp);
            if (eq != std::string::npos) d.location = std::atoi(body.c_str() + eq + 1);
        }
        if (body.find("binding") != std::string::npos) has_binding = true;
        pos = i + 1;
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
}

bool parse_decl_line(const std::string& line, size_t line_index, Decl& d) {
    const std::string body = strip_line_comment(line);
    const std::string t = ltrim(body);
    if (t.empty()) return false;
    if (t[0] == '#' || t[0] == '/' || t[0] == '*') return false;

    size_t pos = 0;
    bool has_binding = false;
    parse_layout_prefix(t, pos, d, has_binding);

    for (;;) {
        while (pos < t.size() && std::isspace(static_cast<unsigned char>(t[pos]))) ++pos;
        const size_t save = pos;
        while (pos < t.size() && (std::isalpha(static_cast<unsigned char>(t[pos])) || t[pos] == '_')) ++pos;
        const std::string kw = t.substr(save, pos - save);
        if (kw == "in") d.dir = Direction::In;
        else if (kw == "out") d.dir = Direction::Out;
        else if (kw == "uniform") d.is_uniform = true;
        else if (kw == "flat") d.flat = true;
        else if (kw == "smooth" || kw == "noperspective" || kw == "centroid" ||
                 kw == "invariant" || kw == "patch" || kw == "coherent" || kw == "readonly" ||
                 kw == "writeonly" || kw == "restrict") { /* 丢弃 */ }
        else { pos = save; break; }
    }

    while (pos < t.size() && std::isspace(static_cast<unsigned char>(t[pos]))) ++pos;
    const size_t tstart = pos;
    while (pos < t.size() && (std::isalnum(static_cast<unsigned char>(t[pos])) || t[pos] == '_')) ++pos;
    const std::string type = t.substr(tstart, pos - tstart);
    if (type.empty()) return false;

    const bool is_sampler = (type.rfind("sampler", 0) == 0) ||
                            (type.rfind("texture", 0) == 0) ||
                            (type.rfind("image", 0) == 0);
    TypeInfo ti;
    if (!is_sampler && !lookup_type(type, ti)) return false;

    while (pos < t.size() && std::isspace(static_cast<unsigned char>(t[pos]))) ++pos;
    const size_t nstart = pos;
    while (pos < t.size() && (std::isalnum(static_cast<unsigned char>(t[pos])) || t[pos] == '_')) ++pos;
    const std::string name = t.substr(nstart, pos - nstart);
    if (name.empty()) return false;

    while (pos < t.size() && std::isspace(static_cast<unsigned char>(t[pos]))) ++pos;
    if (pos < t.size() && t[pos] == '[') {
        const size_t close = t.find(']', pos);
        if (close == std::string::npos) return false;
        d.array_suffix = t.substr(pos, close - pos + 1);
        const std::string inner = trim(t.substr(pos + 1, close - pos - 1));
        d.array_expr = inner;
        if (!inner.empty()) d.array_count = std::atoi(inner.c_str());
        pos = close + 1;
    }

    const std::string rest = trim(t.substr(pos));
    // ★ 支持初始化式 `uniform float uExposure = 1.0;`。
    //   实测缺了这条会让 tonemap.frag 的 24 条 uniform 全部解析失败 →
    //   shaderc 报 "non-opaque uniforms outside a block"。初始化式原文存进
    //   initializer，由 D3D12Shader 求值成 CBV 的默认快照。
    if (!rest.empty() && rest[0] == '=') {
        const size_t semi = rest.rfind(';');
        if (semi == std::string::npos) return false;
        d.initializer = trim(rest.substr(1, semi - 1));
    } else if (rest.empty() || rest[0] != ';') {
        return false;
    }

    d.type = type;
    d.name = name;
    d.line_index = line_index;
    d.is_sampler = is_sampler;
    if (!is_sampler && !d.is_uniform && d.dir == Direction::None) return false;  // const/局部变量等
    return true;
}

// ---------------------------------------------------------------------------
// 一条声明的 location 占用槽位数
//
// ★ 关键：`out mat3 vTBN;` 在 SPIR-V 里占 **3 个** location（矩阵按列展开），
// 数组占 N 个。之前按"每条声明 +1"编号，mat3 后面的 vScreenUV 就被编到
// location 4，跟 mat3 占用的 3/4/5 撞车 → "'location' : overlapping use of
// location 4"。这里按实际槽位数推进。
// ---------------------------------------------------------------------------
// 一条声明的 location 占用槽位数
//
// ★ 关键：`out mat3 vTBN;` 在 SPIR-V 里占 **3 个** location（矩阵按列展开），
// 数组占 N 个。之前按"每条声明 +1"编号，mat3 后面的 vScreenUV 就被编到
// location 4，跟 mat3 占用的 3/4/5 撞车 → "'location' : overlapping use of
// location 4"。这里按实际槽位数推进。
// ---------------------------------------------------------------------------
uint32_t location_slots_for(const Decl& d) {
    uint32_t n = 1;
    if (d.type == "mat2") n = 2;
    else if (d.type == "mat3" || d.type == "mat3x3") n = 3;
    else if (d.type == "mat4" || d.type == "mat4x4") n = 4;
    if (d.array_count > 0) n *= static_cast<uint32_t>(d.array_count);
    return n;
}

// 同类型声明的数组在块里的成员维数（GLSL 数组成员维数 = 构造器维数 + 1）。
// 例如 `uniform vec3 uLightPos[4];` 在块里是 `vec3 uLightPos[4];`（一维），
// 而 `uniform mat4 uCascade[4];` 在块里是 `mat4 uCascade[4];`（一维）。
// HLSL 侧则分别是 float3[4] / float4x4[4]。
std::string block_member_decl(const VulkanizedShader::UniformBlockField& f) {
    std::string s = "    " + f.std140_type + " " + f.name;
    // ★ 保留**源码里原样的**数组后缀（`[4]` 没问题，`[MAX_LIGHTS]` 也必须原样保留：
    // 那是宏，替换成数值反而要额外解析宏表，且 MAX_LIGHTS 就在同一个文件里 define）。
    if (!f.array_suffix.empty()) s += f.array_suffix;
    s += ";\n";
    return s;
}

} // namespace

const char* vertex_semantic_for_location(uint32_t location) {
    // 与 d3d12_mesh::location_to_semantic 保持一致。
    switch (location) {
        case 0: return "POSITION";
        case 1: return "NORMAL";
        case 2: return "TANGENT";
        case 3: return "TEXCOORD";
        case 4: return "COLOR";
        case 5: return "BLENDINDICES";
        case 6: return "BLENDWEIGHT";
        case 7: return "TEXCOORD";
        case 8: return "TEXCOORD";
        default: return nullptr;
    }
}

const char* vertex_semantic_index_for_location(uint32_t location) {
    // 同一语义名出现多次时需要语义索引（TEXCOORD0 / TEXCOORD1 / TEXCOORD2）。
    switch (location) {
        case 3: return "0";
        case 7: return "1";
        case 8: return "2";
        default: return "0";
    }
}

// ---------------------------------------------------------------------------
// GLSL 常量表达式求值（uniform 初始化式）
// ---------------------------------------------------------------------------
namespace {

std::string trim2(const std::string& s) { return trim(s); }

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool parse_scalar(const std::string& t, double& out) {
    const std::string s = trim2(t);
    if (s.empty()) return false;
    if (s == "true") { out = 1.0; return true; }
    if (s == "false") { out = 0.0; return true; }
    std::string body = s;
    // 末尾 c 风格后缀 f/F/u/U（GLSL 里 `1.0f` 合法）
    while (!body.empty() && (body.back() == 'f' || body.back() == 'F' ||
                             body.back() == 'u' || body.back() == 'U')) {
        if (body.size() >= 2 && (body[body.size() - 2] == 'e' || body[body.size() - 2] == 'E')) break;
        body.pop_back();
    }
    if (body.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(body.c_str(), &end);
    if (end != body.c_str() + body.size()) return false;   // 有残留字符 → 不是纯字面量
    out = v;
    return true;
}

} // namespace

bool eval_glsl_const(const std::string& expr_in, const std::string& glsl_type,
                     uint8_t* dst, uint32_t max_bytes, uint32_t& written) {
    written = 0;
    if (!dst) return false;
    const std::string expr = trim2(expr_in);
    if (expr.empty()) return false;

    TypeInfo ti;
    if (!lookup_type(glsl_type, ti)) return false;

    const bool is_matrix = (glsl_type == "mat2" || glsl_type == "mat3" ||
                            glsl_type == "mat4" || glsl_type == "mat3x3" ||
                            glsl_type == "mat4x4");

    // 剥掉构造器名 `vec4(...)` / `mat4(...)`；无括号则整体是一个裸字面量。
    std::string body = expr;
    if (expr.find('(') != std::string::npos) {
        if (expr.back() != ')') return false;
        const size_t lp = expr.find('(');
        body = expr.substr(lp + 1, expr.size() - lp - 2);
    }

    std::vector<double> v;
    for (const std::string& a : split_args(body)) {
        double d = 0.0;
        if (!parse_scalar(a, d)) return false;
        v.push_back(d);
    }
    if (v.empty()) return false;

    // ---- 矩阵 ----
    if (is_matrix) {
        const int n = (glsl_type == "mat2") ? 2
                      : (glsl_type == "mat3" || glsl_type == "mat3x3") ? 3
                                                                       : 4;
        float m[16] = {};
        if (v.size() == 1) {
            for (int i = 0; i < n; ++i) m[i * n + i] = static_cast<float>(v[0]);
        } else if (v.size() == static_cast<size_t>(n * n)) {
            for (int i = 0; i < n * n; ++i) m[i] = static_cast<float>(v[i]);
        } else {
            return false;
        }
        const uint32_t bytes = static_cast<uint32_t>(sizeof(float) * static_cast<size_t>(n * n));
        if (bytes > max_bytes) return false;
        std::memcpy(dst, m, bytes);
        written = bytes;
        return true;
    }

    const bool is_int = (glsl_type == "int" || glsl_type == "uint" || glsl_type == "bool" ||
                         glsl_type.rfind("ivec", 0) == 0 || glsl_type.rfind("uvec", 0) == 0 ||
                         glsl_type.rfind("bvec", 0) == 0);
    const uint32_t comps = (ti.size >= 4) ? (ti.size / 4) : 1;
    if (v.size() != 1 && v.size() != comps) return false;
    if (ti.size > max_bytes) return false;

    for (uint32_t i = 0; i < comps; ++i) {
        const double d = (v.size() == 1) ? v[0] : v[i];
        if (is_int) {
            const int32_t iv = static_cast<int32_t>(d);
            std::memcpy(dst + i * 4, &iv, 4);
        } else {
            const float fv = static_cast<float>(d);
            std::memcpy(dst + i * 4, &fv, 4);
        }
    }
    // vec3 的 std140 槽位是 16 字节，但我们只写 12 —— 末尾 padding 由调用方保持。
    written = comps * 4;
    return true;
}

VulkanizedShader vulkanize_glsl(const std::string& source, GlslVariant variant) {
    VulkanizedShader result;

    const std::string clean = strip_block_comments(source);
    std::vector<std::string> lines;
    {
        std::istringstream iss(clean);
        std::string l;
        while (std::getline(iss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }

    std::vector<Decl> decls;
    for (size_t i = 0; i < lines.size(); ++i) {
        Decl d;
        if (parse_decl_line(lines[i], i, d)) decls.push_back(d);
    }

    // 本 stage 的 interface 声明（in 和 out 分开）：
    //   * vertex  : in  = 顶点属性，out = varying
    //   * fragment: in  = varying，    out = 颜色输出
    //   * compute : 无
    std::vector<Decl*> vertex_inputs;   // 仅 vertex stage
    std::vector<Decl*> varyings_out;    // 仅 vertex stage
    std::vector<Decl*> varyings_in;     // 仅 fragment stage
    std::vector<Decl*> fragment_outs;   // 仅 fragment stage
    for (Decl& d : decls) {
        if (d.is_sampler || d.is_uniform) continue;
        if (variant == GlslVariant::Vertex) {
            if (d.dir == Direction::In) vertex_inputs.push_back(&d);
            else if (d.dir == Direction::Out) varyings_out.push_back(&d);
        } else if (variant == GlslVariant::Fragment) {
            if (d.dir == Direction::In) varyings_in.push_back(&d);
            else if (d.dir == Direction::Out) fragment_outs.push_back(&d);
        }
    }

    // ---- 分配 location ----
    // 顶点输入：优先显式 location，否则按变量名猜语义。
    {
        std::unordered_set<int> used;
        for (Decl* d : vertex_inputs) {
            if (d->location >= 0) used.insert(d->location);
        }
        int next = 0;
        for (Decl* d : vertex_inputs) {
            if (d->location >= 0) continue;
            const std::string& n = d->name;
            auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
            int guess = -1;
            if (has("Pos") || has("pos")) guess = 0;
            else if (has("Normal") || has("normal")) guess = 1;
            else if (has("Tangent") || has("tangent")) guess = 2;
            else if (has("TexCoord") || has("texcoord") || has("uv") || has("UV")) guess = 3;
            else if (has("Color") || has("color")) guess = 4;
            if (guess >= 0 && used.count(guess)) guess = -1;
            if (guess < 0) {
                while (used.count(next)) ++next;
                guess = next;
            }
            used.insert(guess);
            d->location = guess;
        }
        for (Decl* d : vertex_inputs) {
            const char* sem = vertex_semantic_for_location(static_cast<uint32_t>(d->location));
            result.vertex_input_semantics.push_back(sem ? sem : "TEXCOORD");
            result.vertex_input_locations.push_back(static_cast<uint32_t>(d->location));
        }
    }

    // varying：顶点 out 与片元 in **各自独立**从 0 顺序编号，按实际槽位数推进。
    // ★ 前提是两侧声明顺序一致（本工程同名的 varying 在两 stage 里顺序相同）。
    // 用户显式写了 location 的优先，后续顺排从它之后继续。
    {
        auto assign = [](std::vector<Decl*>& list) {
            int next = 0;
            for (Decl* d : list) {
                if (d->location < 0) d->location = next;
                next = d->location + static_cast<int>(location_slots_for(*d));
            }
        };
        assign(varyings_out);
        assign(varyings_in);
    }

    // -----------------------------------------------------------------------
    // 编译期整型常量表（#define + 文件作用域 const）——必须先于 uniform 布局计算
    // -----------------------------------------------------------------------
    // ★ 为什么必须做：`uniform int uLightType[MAX_LIGHTS];` 的维度是宏。
    //   之前 array_count 落到 0、按 1 个元素估算，**块尺寸与真实 std140 布局不符**：
    //   SPIRV-Cross 按真实 GLSL 布局给出 packoffset，而我们算出的偏移偏小 →
    //   数组之后的所有 uniform 全部写错位置（画面错但不崩，极难查）。
    //   所以这里先把常量求出来，数组维度才能算准。
    std::vector<std::string> macro_lines;
    std::unordered_map<std::string, int> const_ints;
    {
        for (const std::string& l : lines) {
            const std::string t = ltrim(strip_line_comment(l));
            if (!starts_with(t, "#define")) continue;
            macro_lines.push_back(strip_line_comment(l));
            // `#define NAME <整数字面量>` → 记进常量表（带符号/十六进制都吃）
            size_t p = 7;
            while (p < t.size() && std::isspace(static_cast<unsigned char>(t[p]))) ++p;
            const size_t ns = p;
            while (p < t.size() && (std::isalnum(static_cast<unsigned char>(t[p])) || t[p] == '_')) ++p;
            if (p == ns) continue;
            const std::string nm = t.substr(ns, p - ns);
            const std::string val = trim(t.substr(p));
            double dv = 0.0;
            if (parse_scalar(val, dv)) const_ints[nm] = static_cast<int>(dv);
        }
    }
    // 文件作用域 `const int NAME = <字面量>;` 提升到 #version 之后（并删除原行：
    // 同名 const 重复定义非法，不像 #define 可以重放）。同时记进常量表。
    std::unordered_set<size_t> hoisted_const_lines;
    {
        static const char* kScalarTypes[] = {"int", "uint", "float", "bool"};
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string raw = strip_line_comment(lines[i]);
            if (raw.empty() || std::isspace(static_cast<unsigned char>(raw[0]))) continue;
            if (!starts_with(raw, "const")) continue;
            size_t p = 5;
            if (p >= raw.size() || !std::isspace(static_cast<unsigned char>(raw[p]))) continue;
            while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p]))) ++p;
            const size_t ts = p;
            while (p < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[p])) || raw[p] == '_')) ++p;
            const std::string ty = raw.substr(ts, p - ts);
            bool known = false;
            for (const char* k : kScalarTypes) { if (ty == k) { known = true; break; } }
            if (!known) continue;
            while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p]))) ++p;
            const size_t ns = p;
            while (p < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[p])) || raw[p] == '_')) ++p;
            if (p == ns) continue;   // 没有变量名（不是 `const T name = ...`）
            const std::string nm = raw.substr(ns, p - ns);

            // ★ 全部用 raw 的**绝对下标**（曾用 rest 的相对下标去切 raw，
            //   把初值截断成 `const float PI =` → shaderc 报
            //   "syntax error, unexpected LAYOUT"，极难定位）。
            const size_t eq = raw.find('=', p);
            if (eq == std::string::npos) continue;
            const size_t semi_abs = raw.find(';', eq);
            if (semi_abs == std::string::npos) continue;
            const std::string expr = trim(raw.substr(eq + 1, semi_abs - eq - 1));
            if (expr.empty()) continue;
            // 记录整数值（供给数组维度用）
            double dv = 0.0;
            if (parse_scalar(expr, dv)) const_ints[nm] = static_cast<int>(dv);
            // 初始化式必须是纯字面量（数字 + 符号 + 小数点 + 指数），
            // 否则它可能依赖别的声明，提前会让语义变化。
            bool literal_only = true;
            for (char c : expr) {
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '+' ||
                      c == '-' || c == 'e' || c == 'E' || c == 'x' || c == 'X' ||
                      std::isxdigit(static_cast<unsigned char>(c)) || c == 'u' || c == 'U')) {
                    literal_only = false;
                    break;
                }
            }
            if (!literal_only) continue;
            hoisted_const_lines.insert(i);
            macro_lines.push_back(trim(raw.substr(0, semi_abs + 1)));
        }
    }

    // ---- uniform 块布局（std140）----
    std::vector<VulkanizedShader::UniformBlockField> fields;
    uint32_t block_size = 0;
    for (const Decl& d : decls) {
        if (!d.is_uniform || d.is_sampler) continue;
        TypeInfo ti;
        if (!lookup_type(d.type, ti)) continue;
        VulkanizedShader::UniformBlockField f;
        f.type = ti.hlsl;
        f.name = d.name;
        f.std140_type = d.type;
        f.array_suffix = d.array_suffix;
        f.initializer = d.initializer;
        if (!d.array_suffix.empty()) {
            // std140：数组元素步长 = align_up(align_up(size, align), 16)
            const uint32_t stride = align_up(align_up(ti.size, ti.align), 16);
            f.offset = align_up(block_size, 16);
            // 维度：数字直接用；标识符查常量表（宏或 const）。查不到就按 1 占位
            // （块尺寸会偏小，这是已知局限 —— 但本工程全部维度都能解析出来）。
            uint32_t n = 0;
            if (d.array_count > 0) {
                n = static_cast<uint32_t>(d.array_count);
            } else {
                auto it = const_ints.find(d.array_expr);
                if (it != const_ints.end() && it->second > 0) {
                    n = static_cast<uint32_t>(it->second);
                }
            }
            if (n == 0) n = 1;
            f.size = stride * n;
            f.array_count = n;
            block_size = f.offset + f.size;
        } else {
            f.offset = align_up(block_size, ti.align);
            f.size = ti.size;
            block_size = f.offset + ti.size;
        }
        fields.push_back(f);
    }
    block_size = align_up(block_size, 16);

    // ---- 重建源码 ----
    std::ostringstream out;
    bool wrote_version = false;
    bool wrote_uniform_block = false;

    size_t first_uniform_line = std::string::npos;
    for (const Decl& d : decls) {
        if (d.is_uniform && !d.is_sampler) { first_uniform_line = d.line_index; break; }
    }

    // ★ `gl_InstanceID` 在 Vulkan GLSL 里不存在（改名 `gl_InstanceIndex`）。
    //   instanced.vert 用 `dst_models[gl_InstanceID]` 索引 GPU 剔除后的 SSBO，
    //   不改写则 shaderc 报 "'gl_InstanceID' : undeclared identifier
    //   (Did you mean gl_InstanceIndex?)" + "'[]' : scalar integer expression required"。
    auto rewrite_builtins = [](std::string s) {
        static const std::pair<const char*, const char*> kMap[] = {
            {"gl_InstanceID", "gl_InstanceIndex"},
        };
        for (const auto& [from, to] : kMap) {
            const size_t n = std::strlen(from);
            for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + n)) {
                s.replace(p, n, to);
            }
        }
        return s;
    };

    // ★★ SSBO 的 binding 必须整体搬到高位 —— 这是 D3D12 侧的结构性硬约束。
    //
    //   背景：D3D12 的描述符寄存器空间**只有一个**（t/s/u/b 各一条线），而
    //   GLSL/Vulkan 是 (set, binding) 二维的。vulkanize 把采样器排在
    //   `set=0, binding=1..N`（0 留给 Globals），SPIRV-Cross 又把 binding 号
    //   原样当寄存器号 → 采样器落在 t1..tN。
    //
    //   原样透传的 SSBO 块（如 instanced.vert 的 `binding = 2`）于是也落在 t2，
    //   和 frag 的采样器**抢同一个寄存器**：同一个 t2，VS 期望它是
    //   ByteAddressBuffer、PS 期望它是 Texture2D。引擎侧一张 SRV 表只能放一种
    //   描述符类型 → 描述符与着色器声明不符 → Intel Arc 上直接
    //   DEVICE_REMOVED/DEVICE_HUNG，且**是否触发取决于当帧材质绑了几张纹理**
    //   （表现为分辨率/场景相关的不确定性），极难定位。
    //
    //   解法：把 SSBO 的 binding 统一 +kStorageBindingBase 搬到采样器区间之外，
    //   两个区间永不重叠。改的是**中间产物**（vulkanize 输出），原始 .vert/.comp
    //   一行不动 → GL/Vulkan 主路径零影响。
    auto rewrite_ssbo_binding = [](const std::string& s) -> std::string {
        if (s.find("std430") == std::string::npos) return s;
        const size_t bp = s.find("binding");
        if (bp == std::string::npos) return s;
        const size_t eq = s.find('=', bp);
        if (eq == std::string::npos) return s;
        size_t p = eq + 1;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
        size_t q = p;
        while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) ++q;
        if (q == p) return s;
        const long orig = std::strtol(s.substr(p, q - p).c_str(), nullptr, 10);
        if (orig < 0 || orig >= kStorageBindingBase) return s;   // 已搬过 / 非法 → 原样
        std::string r = s;
        r.replace(p, q - p, std::to_string(kStorageBindingBase + orig));
        return r;
    };

    // 采样器顺序号（binding 0 保留给 Globals 块）。
    std::map<size_t, int> sampler_binding;   // line index → binding
    {
        int idx = 0;
        for (const Decl& d : decls) {
            if (!d.is_sampler) continue;
            sampler_binding[d.line_index] = 1 + idx;
            ++idx;
        }
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string body = strip_line_comment(lines[i]);

        if (!wrote_version && starts_with(ltrim(body), "#version")) {
            // Vulkan-GLSL 必须 450+，且不接受 core/compatibility profile 关键字。
            out << "#version 450\n";
            // 紧接着重放所有 #define，保证 uniform 块里引用的宏已可见。
            for (const std::string& m : macro_lines) out << m << "\n";
            wrote_version = true;
            continue;
        }

        const Decl* decl = nullptr;
        for (const Decl& d : decls) {
            if (d.line_index == i) { decl = &d; break; }
        }
        if (!decl) {
            // 已被提到文件头的 const 不再原地输出（重复定义非法）。
            if (hoisted_const_lines.count(i)) continue;
            // SSBO 块的行（`layout(std430, binding = N) readonly buffer X {`）
            // 不属于任何可识别 decl，走这条透传路径 —— 必须在这里重映射 binding。
            out << rewrite_ssbo_binding(lines[i]) << "\n";
            continue;
        }

        const std::string indent = leading_indent(lines[i]);

        if (decl->is_uniform && !decl->is_sampler) {
            // 第一条普通 uniform 处插入整个 Globals 块。
            if (!wrote_uniform_block && decl->line_index == first_uniform_line) {
                out << "layout(set = 0, binding = 0, std140) uniform Globals {\n";
                for (const auto& f : fields) {
                    // ★ 成员类型必须用 **GLSL 类型名**（vec3/mat4），不是 HLSL 名。
                    // 之前误写 HLSL 名（float3/float4x4）→ shaderc 报
                    // "syntax error, unexpected IDENTIFIER"。HLSL 名只在
                    // 给 D3D12 侧建 root signature 时用（UniformBlockField::type）。
                    out << block_member_decl(f);
                }
                out << "};\n";
                wrote_uniform_block = true;
            }
            continue;   // 该行已并入块
        }

        if (decl->is_sampler) {
            const auto it = sampler_binding.find(decl->line_index);
            const int binding = (it != sampler_binding.end()) ? it->second : 1;
            out << indent << "layout(set = 0, binding = " << binding << ") uniform "
                << decl->type << " " << decl->name << decl->array_suffix << ";\n";
            continue;
        }

        // ---- in / out ----
        const char* dir = "in";
        if (variant == GlslVariant::Vertex) {
            dir = (decl->dir == Direction::In) ? "in" : "out";
        } else if (variant == GlslVariant::Fragment) {
            dir = (decl->dir == Direction::In) ? "in" : "out";
        }
        out << indent << "layout(location = " << (decl->location < 0 ? 0 : decl->location) << ") "
            << (decl->flat ? "flat " : "") << dir << " " << decl->type << " " << decl->name
            << decl->array_suffix << ";\n";
    }

    result.source = rewrite_builtins(out.str());
    result.uniform_fields = std::move(fields);
    result.uniform_block_size = block_size;
    for (const Decl& d : decls) {
        if (d.is_sampler) result.sampler_names.push_back(d.name);
    }
    result.ok = true;
    return result;
}

} // namespace gryce_engine::render::d3d12
