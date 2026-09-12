# 文件名：reflection.h

## 文件作用

这个文件是整个反射系统的说明书（头文件），负责让程序在运行时能查到自己有哪些类型、每个类型有哪些字段。它声明了字段类型清单 FieldType、单个字段的信息 FieldInfo、一个类型的信息 TypeInfo，以及装所有类型的"总账本" Registry 和填字段用的 TypeBuilder。文件末尾还提供了 GRYCE_REFLECT_CLASS 这一整组宏，方便在代码里一行行登记某个类的字段。最后还声明了一个叫 register_builtin_reflections 的入口函数。

---