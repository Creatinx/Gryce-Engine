#ifndef GRYCE_KEY_CODES_H
#define GRYCE_KEY_CODES_H

// 键码常量（与 GLFW_KEY_* 同一编号）。
//
// 为什么单独列一份：输入层、脚本 API、UI 的 UIInputFrame 都用 GLFW 编号，
// 但绕过平台层自建窗口的宿主（如 GryceRenderSmoke）不该为了几个常量去拉
// GLFW 头文件与它的 include 路径。硬编码 256/257 这类裸数字又会读不懂。
// 只列输入层真正转发的那些键；其余按键用 GInput_InjectKey 传对应 GLFW 值
// 同样有效。
enum GKeyCode {
    GKEY_SPACE      = 32,
    GKEY_A          = 65,
    GKEY_C          = 67,
    GKEY_V          = 86,
    GKEY_X          = 88,
    GKEY_Z          = 90,
    GKEY_ESCAPE     = 256,
    GKEY_ENTER      = 257,
    GKEY_TAB        = 258,
    GKEY_BACKSPACE  = 259,
    GKEY_INSERT     = 260,
    GKEY_DELETE     = 261,
    GKEY_RIGHT      = 262,
    GKEY_LEFT       = 263,
    GKEY_DOWN       = 264,
    GKEY_UP         = 265,
    GKEY_PAGE_UP    = 266,
    GKEY_PAGE_DOWN  = 267,
    GKEY_HOME       = 268,
    GKEY_END        = 269,
    GKEY_F1         = 290,
    GKEY_F2         = 291,
    GKEY_F3         = 292,
    GKEY_F4         = 293,
    GKEY_F5         = 294,
    GKEY_F6         = 295,
    GKEY_F7         = 296,
    GKEY_F8         = 297,
    GKEY_F9         = 298,
    GKEY_F10        = 299,
    GKEY_F11        = 300,
    GKEY_F12        = 301,
    GKEY_LEFT_SHIFT = 340,
    GKEY_LEFT_CTRL  = 341,
    GKEY_LEFT_ALT   = 342
};

#endif
