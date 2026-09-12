# 文件名：script_vm.h

## 文件作用

这个文件是干什么的：它声明了 ScriptVM 类，以及配套的 ScriptResult 和 JSValueWrapper 两个小类型。ScriptVM 是对 QuickJS 的一层包装，负责跑 JavaScript：可以初始化、关闭、执行代码、按模块方式执行、编译出字节码、加载字节码、调用模块导出的函数、调用全局函数，还可以把 C++ 函数注册成 JS 里的全局函数。ScriptResult 用来装一次运行的结果（成没成、返回内容、错误信息），JSValueWrapper 用来把 C++ 侧的数据（数字、字符串、布尔等）装好再传给脚本。

---