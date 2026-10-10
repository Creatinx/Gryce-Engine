#include "d3d12_reflect.h"

#include <cctype>
#include <cstdlib>

namespace gryce_engine::render::d3d12 {

namespace {

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 去掉 // 与 /* */ 注释。DXC/SPIRV-Cross 的输出里注释很少，但 GLSL 源里
// 常见，留着会让 "buffer" 这种词在注释里被误判成声明。
std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            if (i < s.size()) out.push_back('\n');
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

// 从一段文本里取 "名 = 数字"，用于解析 layout(std430, binding = 3)。
// 成功时返回 true 并写入 out。
bool parse_int_assignment(const std::string& text, const char* key, uint32_t& out) {
    const size_t kp = text.find(key);
    if (kp == std::string::npos) return false;
    size_t eq = text.find('=', kp + std::char_traits<char>::length(key));
    if (eq == std::string::npos) return false;
    size_t i = eq + 1;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    uint32_t v = 0;
    bool any = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        v = v * 10u + static_cast<uint32_t>(text[i] - '0');
        ++i;
        any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

// ---- GLSL 侧：`layout(std430, binding = N) [readonly|writeonly] buffer <Block> {...}`
//
// ★ 关键（2026-10-09 实测确认）：SPIR-V 在 shaderc 之后 **不带 OpName**（HLSL 里
// 的变量名全是 `_28` / `_329` 这种 ID 回退名），所以**不能靠名字把 GLSL 块与 HLSL
// 变量对上**。能对上的是 **binding 号**：SPIRV-Cross 把 GLSL `binding = N` 原样
// 映射成 `register(tN)`（只读 SSBO）或 `register(uN)`（可写 SSBO）。
// 实测：SrcModels b0→t0、SrcSpheres b1→t1、DstModels b2→u2、DrawArgsBuffer b3→u3、
// CullParams b4→t4、HiZLevels b5→t5；instanced.vert 的 DstModels b2→t2（只读）。
struct GlslBlock {
    std::string name;
    std::string member;
    uint32_t binding = 0xFFFFFFFFu;
};

std::vector<GlslBlock> parse_glsl_blocks(const std::string& glsl_raw) {
    const std::string glsl = strip_comments(glsl_raw);
    std::vector<GlslBlock> out;
    const std::string key = "buffer";
    for (size_t p = glsl.find(key); p != std::string::npos; p = glsl.find(key, p + 1)) {
        // 必须是独立的词（前面不能是标识符字符）。
        if (p > 0 && is_ident_char(glsl[p - 1])) continue;
        size_t q = p + key.size();
        if (q < glsl.size() && is_ident_char(glsl[q])) continue;   // 例如 buffered
        while (q < glsl.size() && std::isspace(static_cast<unsigned char>(glsl[q]))) ++q;
        const size_t ns = q;
        while (q < glsl.size() && is_ident_char(glsl[q])) ++q;
        if (q == ns) continue;
        GlslBlock b;
        b.name = glsl.substr(ns, q - ns);

        // binding 号：在同一个"语句"前缀里（上一个 ; } { 之后）找 layout(...)。
        size_t stmt = p;
        while (stmt > 0 && glsl[stmt - 1] != ';' && glsl[stmt - 1] != '}' &&
               glsl[stmt - 1] != '{') {
            --stmt;
        }
        const std::string prefix = glsl.substr(stmt, p - stmt);
        const size_t lp = prefix.rfind("layout");
        if (lp != std::string::npos) {
            const size_t op = prefix.find('(', lp);
            const size_t cp = (op == std::string::npos) ? std::string::npos
                                                        : prefix.find(')', op);
            if (op != std::string::npos && cp != std::string::npos && cp > op) {
                uint32_t v = 0;
                if (parse_int_assignment(prefix.substr(op + 1, cp - op - 1), "binding", v)) {
                    b.binding = v;
                }
            }
        }

        // 块内第一个成员名（诊断用）。
        const size_t brace = glsl.find('{', q);
        if (brace != std::string::npos) {
            const size_t semi = glsl.find(';', brace);
            if (semi != std::string::npos) {
                const std::string first = glsl.substr(brace + 1, semi - brace - 1);
                size_t e = first.size();
                while (e > 0 && (std::isspace(static_cast<unsigned char>(first[e - 1])) ||
                                 first[e - 1] == ']')) {
                    if (first[e - 1] == '[') {
                        while (e > 0 && first[e - 1] != '[') --e;
                        --e;
                        continue;
                    }
                    --e;
                }
                size_t s = e;
                while (s > 0 && is_ident_char(first[s - 1])) --s;
                b.member = first.substr(s, e - s);
            }
        }
        out.push_back(std::move(b));
    }
    return out;
}

// ---- HLSL 侧：`<RW>StructuredBuffer<T> NAME : register([tu]N);` -----------------
struct HlslVar {
    std::string name;
    std::string elem_type;
    uint32_t reg = 0;
    uint32_t stride = 0;
    bool is_uav = false;
};

// HLSL 内建标量/向量/矩阵的字节数（0 = 未知）。StructuredBuffer 的 T 会命中这里，
// 或者是一个本文件解析出来的 struct。
uint32_t hlsl_builtin_size(const std::string& t) {
    // 标量
    if (t == "bool" || t == "int" || t == "uint" || t == "float" || t == "dword") return 4;
    if (t == "double") return 8;
    if (t == "half") return 2;
    if (t == "int64_t" || t == "uint64_t") return 8;
    // 向量 floatN / intN / uintN / boolN
    const size_t npos = t.find_last_not_of("0123456789");
    if (npos != std::string::npos) {
        const std::string base = t.substr(0, npos + 1);
        const std::string digits = t.substr(npos + 1);
        const uint32_t n = digits.empty() ? 1u : static_cast<uint32_t>(std::atoi(digits.c_str()));
        uint32_t elem = 0;
        if (base == "float") elem = 4;
        else if (base == "int" || base == "uint" || base == "bool" || base == "dword") elem = 4;
        else if (base == "double") elem = 8;
        else if (base == "half") elem = 2;
        if (elem && n >= 1 && n <= 4) return elem * n;
        // 矩阵 float4x4 等
        const size_t x = base.find('x');
        if (x != std::string::npos) {
            const std::string rb = base.substr(0, x);
            uint32_t re = 0;
            if (rb == "float") re = 4;
            else if (rb == "double") re = 8;
            else if (rb == "half") re = 2;
            if (re) {
                const uint32_t rows = 4u;   // 本工程只用 float4x4 / float3x3 等方阵
                const uint32_t cols = n;
                if (rows == 4 && cols == 4) return re * 16u;
                if (rows == 4 && cols == 3) return re * 12u;
            }
        }
    }
    return 0;
}

// 求 HLSL struct 的字节数：解析该结构体所有成员，按自然对齐累加。
// 本工程的 SSBO 结构（cull_instances.comp 的 CullParams / DrawArgsBuffer）
// 成员全是 4 字节标量或 16 字节向量/矩阵，按"成员大小对齐累加"就能得到
// std430 的正确结果（实测 416 / 16）。
uint32_t hlsl_struct_size(const std::string& hlsl, const std::string& name) {
    const std::string key = "struct " + name;
    size_t p = hlsl.find(key);
    if (p == std::string::npos) return 0;
    size_t brace = hlsl.find('{', p);
    if (brace == std::string::npos) return 0;
    size_t end = hlsl.find('}', brace);
    if (end == std::string::npos) return 0;
    const std::string body = hlsl.substr(brace + 1, end - brace - 1);

    uint32_t total = 0;
    size_t i = 0;
    while (i < body.size()) {
        const size_t semi = body.find(';', i);
        if (semi == std::string::npos) break;
        std::string decl = body.substr(i, semi - i);
        i = semi + 1;
        // 去掉行注释与数组后缀
        const size_t slash = decl.find("//");
        if (slash != std::string::npos) decl = decl.substr(0, slash);
        uint32_t count = 1;
        const size_t br = decl.find('[');
        if (br != std::string::npos) {
            const size_t close = decl.find(']', br);
            if (close != std::string::npos) {
                count = static_cast<uint32_t>(std::atoi(decl.substr(br + 1, close - br - 1).c_str()));
                if (count == 0) count = 1;
                decl = decl.substr(0, br);
            }
        }
        // 取第一个词 = 类型
        size_t s = 0;
        while (s < decl.size() && std::isspace(static_cast<unsigned char>(decl[s]))) ++s;
        size_t e = s;
        while (e < decl.size() && is_ident_char(decl[e])) ++e;
        if (e == s) continue;
        uint32_t sz = hlsl_builtin_size(decl.substr(s, e - s));
        total += sz * count;
    }
    return total;
}

std::vector<HlslVar> parse_hlsl_ssbo(const std::string& hlsl) {
    std::vector<HlslVar> out;
    for (size_t p = hlsl.find("register("); p != std::string::npos; p = hlsl.find("register(", p + 1)) {
        size_t q = p + 9;
        if (q >= hlsl.size()) continue;
        const char kind = hlsl[q];
        if (kind != 't' && kind != 'u') continue;
        ++q;
        uint32_t v = 0;
        bool any = false;
        while (q < hlsl.size() && std::isdigit(static_cast<unsigned char>(hlsl[q]))) {
            v = v * 10u + static_cast<uint32_t>(hlsl[q] - '0');
            ++q;
            any = true;
        }
        if (!any) continue;

        // 该语句的开头：往回找上一个 ';' 或 '{' 或 '}'。
        size_t stmt = p;
        while (stmt > 0 && hlsl[stmt - 1] != ';' && hlsl[stmt - 1] != '}' && hlsl[stmt - 1] != '{') --stmt;
        const std::string stmt_text = hlsl.substr(stmt, p - stmt);
        const size_t sb = stmt_text.find("StructuredBuffer");
        const size_t bab = stmt_text.find("ByteAddressBuffer");
        if (sb == std::string::npos && bab == std::string::npos) continue;
        const bool is_uav = stmt_text.find("RWStructuredBuffer") != std::string::npos ||
                            stmt_text.find("RWByteAddressBuffer") != std::string::npos;

        // 变量名 = ':' 之前最后一个标识符。
        size_t e = stmt_text.size();
        while (e > 0 && (std::isspace(static_cast<unsigned char>(stmt_text[e - 1])) ||
                         stmt_text[e - 1] == ':')) {
            --e;
        }
        size_t s = e;
        while (s > 0 && is_ident_char(stmt_text[s - 1])) --s;
        if (s == e) continue;

        HlslVar hv;
        hv.name = stmt_text.substr(s, e - s);
        hv.reg = v;
        hv.is_uav = is_uav;
        // 元素类型：尖括号内
        const size_t lt = stmt_text.find('<');
        const size_t gt = stmt_text.find('>', lt == std::string::npos ? 0 : lt);
        if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1) {
            hv.elem_type = trim(stmt_text.substr(lt + 1, gt - lt - 1));
        }
        out.push_back(std::move(hv));
    }

    // 元素字节数：内建类型直接查表，否则找 struct 定义累加。
    for (HlslVar& hv : out) {
        if (hv.elem_type.empty()) continue;
        uint32_t sz = hlsl_builtin_size(hv.elem_type);
        if (sz == 0) sz = hlsl_struct_size(hlsl, hv.elem_type);
        hv.stride = sz;
    }
    return out;
}


std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

SsboReflection reflect_ssbo_bindings(const std::string& hlsl, const std::string& vulkan_glsl) {
    SsboReflection r;
    const std::vector<GlslBlock> blocks = parse_glsl_blocks(vulkan_glsl);
    const std::vector<HlslVar> vars = parse_hlsl_ssbo(hlsl);

    // 主路径：按 **binding 号 == 寄存器号** 配对（见文件头说明）。
    for (const GlslBlock& gb : blocks) {
        if (gb.binding == 0xFFFFFFFFu) continue;
        const HlslVar* hv = nullptr;
        for (const HlslVar& v : vars) {
            if (v.reg == gb.binding) { hv = &v; break; }
        }
        if (!hv) continue;   // 该块在 HLSL 里不存在（被优化掉）→ 忽略
        SsboBinding b;
        b.block = gb.name;
        b.member = gb.member;
        b.hlsl_var = hv->name;
        b.hlsl_type = hv->elem_type;
        b.reg = hv->reg;
        b.is_uav = hv->is_uav;
        b.is_raw = hv->elem_type.empty();   // ByteAddressBuffer：没有 <T> → raw
        b.stride = b.is_raw ? 4u : hv->stride;
        r.bindings.push_back(std::move(b));
    }

    // 兜底：GLSL 侧没解析出 binding（例如上游换了输入），退回按名字配对。
    if (r.bindings.empty()) {
        for (const HlslVar& hv : vars) {
            SsboBinding b;
            b.hlsl_var = hv.name;
            b.hlsl_type = hv.elem_type;
            b.reg = hv.reg;
            b.is_uav = hv.is_uav;
            b.is_raw = hv.elem_type.empty();
            b.stride = b.is_raw ? 4u : hv.stride;
            for (const GlslBlock& gb : blocks) {
                if (hv.name == gb.name || (!gb.member.empty() && hv.name == gb.member)) {
                    b.block = gb.name;
                    b.member = gb.member;
                    break;
                }
            }
            if (b.block.empty()) b.block = hv.name;   // 退化：用 HLSL 变量名当键
            r.bindings.push_back(std::move(b));
        }
    }

    for (const SsboBinding& b : r.bindings) {
        const uint32_t need = b.reg + 1u;
        if (b.is_uav) {
            if (need > r.uav_count) r.uav_count = need;
        } else {
            if (need > r.srv_count) r.srv_count = need;
        }
    }
    return r;
}

const SsboBinding* find_ssbo(const std::vector<SsboBinding>& list, const std::string& name) {
    for (const SsboBinding& b : list) {
        if (b.block == name) return &b;
    }
    const std::string lower = to_lower(name);
    for (const SsboBinding& b : list) {
        if (to_lower(b.block) == lower) return &b;
    }
    for (const SsboBinding& b : list) {
        if (!b.hlsl_var.empty() && to_lower(b.hlsl_var) == lower) return &b;
    }
    return nullptr;
}

} // namespace gryce_engine::render::d3d12
