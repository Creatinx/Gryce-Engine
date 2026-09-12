# 文件名：big_bridge.h

## 文件作用

这个文件是干什么的：它只声明了一个让外面用的注册函数，用来把 big 这个对象提供给 JS 脚本。它说明这段绑定是从 Lua 的 big_number 模块改过来的，给脚本提供 BigInt 和 BigDecimal 两类功能，并给出用法例子，比如 big.int("123...")、big.decimal("3.14...") 这种写法。

---