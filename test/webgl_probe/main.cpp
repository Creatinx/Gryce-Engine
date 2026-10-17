// M0 WebGL2 环境探针（最小 GLFW + WebGL2 程序）
// 目的：在把引擎接进来之前，先在真实浏览器里核实：
//   1. GLFW Emscripten 端口能建出 WebGL2 context；
//   2. EXT_color_buffer_float（HDR RGBA16F 渲染目标）是否可用；
//   3. 深度纹理 / shadow compare 模式是否可用（阴影 atlas 依赖）；
//   4. 桌面 GL 的 DSA 入口（glCreateTextures 等）在 WebGL2 确实不存在（验证 shim 必要性）；
//   5. --preload-file 的 MEMFS 文件能否用 fopen 读到；
//   6. 三角形能画出来且 readPixels 非背景色（渲染通路 + 帧内回读可行）。
// 结果同时输出到 console.log 与 DOM（#probe-results），后者供无头 Chrome
// `--dump-dom` 读回，实现文本级断言。
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <GLFW/glfw3.h>
#include <GLES3/gl3.h>

#include <emscripten.h>

static std::string g_results;
static int g_fail = 0;

static void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_results += buf;
    g_results += "\n";
}

static bool has_ext(const char* name) {
    // WebGL2 下 glGetString(GL_EXTENSIONS) 返回以空格分隔的扩展名列表
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!exts) return false;
    std::string all(exts);
    return all.find(name) != std::string::npos;
}

// 把结果刷进 DOM（EM_ASM 在编译期由 Emscripten 处理，仅在 __EMSCRIPTEN__ 可用）
static void flush_dom() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var el = document.getElementById('probe-results');
        if (el) { el.textContent = UTF8ToString($0); }
        document.title = 'PROBE_DONE fails=' + $1;
    }, g_results.c_str(), g_fail);
#endif
}

static const char* kVert =
    "#version 300 es\n"
    "in vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char* kFrag =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = vec4(0.9, 0.3, 0.2, 1.0); }\n";

int main() {
    if (!glfwInit()) {
        log_line("FAIL glfwInit");
        flush_dom();
        return 1;
    }
    // WebGL2 context（GLFW Emscripten 端口：这些 hint 映射到 WebGL context 属性）
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(320, 240, "gryce webgl probe", nullptr, nullptr);
    if (!win) {
        log_line("FAIL glfwCreateWindow (WebGL2 context)");
        flush_dom();
        return 1;
    }
    glfwMakeContextCurrent(win);

    log_line("GL_VERSION=%s", glGetString(GL_VERSION));
    log_line("GL_RENDERER=%s", glGetString(GL_RENDERER));
    log_line("GLSL=%s", glGetString(GL_SHADING_LANGUAGE_VERSION));

    // --- 扩展探测 ---
    struct { const char* name; bool need; } exts[] = {
        {"EXT_color_buffer_float", true},   // HDR RGBA16F 渲染目标
        {"OES_texture_float_linear", false}, // RGBA32F 线性过滤（引擎基本不用）
        {"EXT_texture_filter_anisotropic", false},
        {"EXT_disjoint_timer_query_webgl2", false}, // GPU 计时（首版关）
        {"WEBGL_multisampled_render_to_texture", false}, // MSAA resolve 备选
    };
    for (auto& e : exts) {
        bool ok = has_ext(e.name);
        log_line("EXT %s = %s", e.name, ok ? "yes" : "no");
        if (e.need && !ok) { log_line("FAIL required ext missing: %s", e.name); ++g_fail; }
    }

    // --- RGBA16F FBO 完整性 ---
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 64, 64);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    log_line("FBO RGBA16F status = 0x%x (%s)", status,
             status == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE");
    if (status != GL_FRAMEBUFFER_COMPLETE) ++g_fail;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // --- 深度纹理 + COMPARE 模式（阴影 sampler） ---
    GLuint dtex = 0, dfbo = 0;
    glGenTextures(1, &dtex);
    glBindTexture(GL_TEXTURE_2D, dtex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, 64, 64);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &dfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, dfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);
    GLenum dstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    log_line("FBO DEPTH24 status = 0x%x (%s)", dstatus,
             dstatus == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE");
    if (dstatus != GL_FRAMEBUFFER_COMPLETE) ++g_fail;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // --- DSA 入口存在性（预期全部缺失 → shim 必要性确认） ---
    // 不能直接用 glfwGetProcAddress：Emscripten 的 GLFW 端口对它支持不全，
    // 桌面/Web 都会返回 null。改为在 JS 侧查 WebGL context 对象的函数名
    // （WebGL 的函数挂在 context 上且无 "gl" 前缀，DSA 入口在 WebGL2 不存在）。
    int dsa_create_tex = EM_ASM_INT({
        var gl = GL.context;
        return (gl && typeof gl.createTextures === 'function') ? 1 : 0;
    });
    int dsa_named_fbo = EM_ASM_INT({
        var gl = GL.context;
        return (gl && typeof gl.namedFramebufferTexture === 'function') ? 1 : 0;
    });
    log_line("DSA glCreateTextures present = %s", dsa_create_tex ? "yes" : "no");
    log_line("DSA glNamedFramebufferTexture present = %s", dsa_named_fbo ? "yes" : "no");
    if (dsa_create_tex || dsa_named_fbo) {
        log_line("WARN unexpected DSA presence (context is not WebGL2?)");
    }

    // --- MEMFS preload 文件读取 ---
    {
        FILE* f = fopen("probe_asset.txt", "rb");
        if (f) {
            char buf[256] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            log_line("MEMFS read %zu bytes: %s", n, buf);
        } else {
            log_line("WARN MEMFS probe_asset.txt not found (preload missing?)");
        }
    }

    // --- 三角形渲染 + 帧内回读 ---
    GLuint vbo = 0, vao = 0, vs = 0, fs = 0, prog = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    float tri[] = {-0.6f, -0.5f, 0.6f, -0.5f, 0.0f, 0.6f};
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVert, nullptr);
    glCompileShader(vs);
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kFrag, nullptr);
    glCompileShader(fs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(vs, 512, nullptr, log); log_line("FAIL vert: %s", log); ++g_fail; }
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(fs, 512, nullptr, log); log_line("FAIL frag: %s", log); ++g_fail; }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(prog, 512, nullptr, log); log_line("FAIL link: %s", log); ++g_fail; }

    glViewport(0, 0, 320, 240);
    glClearColor(0.1f, 0.12f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 帧内回读（引擎 capture_frame_rgba 同款路径）
    unsigned char px[4] = {0};
    glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    bool tri_hit = px[0] > 150 && px[1] < 120;
    log_line("readPixels center = (%u,%u,%u,%u) triangle_visible=%s",
             px[0], px[1], px[2], px[3], tri_hit ? "yes" : "no");
    if (!tri_hit) ++g_fail;

    GLenum err = glGetError();
    log_line("glGetError = 0x%x", err);

    log_line(g_fail == 0 ? "PROBE_RESULT PASS" : "PROBE_RESULT FAIL");
    flush_dom();

    // 不进入主循环：探针是单帧程序，结果已写入 DOM。保持窗口一帧让截图生效。
    glfwSwapBuffers(win);
    return 0;
}
