# 文件名：builtin_reflections.cpp

## 文件作用

这个文件是"反射登记"真正动手填内容的地方。它把引擎里做好的大量组件类型（比如 Transform、Camera、Light、各类碰撞体、2D/3D 渲染组件等），以及每个组件里需要让编辑器修改的字段，用 GRYCE_REFLECT_CLASS 这一组宏统一登记进反射账本。这样在编辑器里选中对象时，面板就能自动列出它的属性供人修改。文件末尾的 register_builtin_reflections 函数是个入门后的检查点，它确认账号里确实登记了类型。

---