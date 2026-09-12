# 文件名：engine_bridge.h

## 文件作用

这个文件是干什么的：它声明了一个注册函数，用来把大块的 engine 功能提供给 JS 脚本。它在注释里列出了 engine 对象下面会有的子段：log（日志）、version（版本）、self（当前实体）、entity（实体操作）、component（组件操作）、state（跨场景状态）、input（输入）、time（时间）、scene（场景）、audio（声音）、fx（特效）、json（读配置）、physics（物理），以及一些界面相关的函数。这个文件只是对外给出注册入口的说明。

---