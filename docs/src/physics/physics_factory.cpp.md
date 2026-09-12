# 文件名：physics_factory.cpp

## 文件作用

这个文件实现了物理世界的"工厂"：create_physics_world_2d 和 create_physics_world_3d 两个函数。它根据调用者传进来的名字来决定造出哪个物理世界。2D 的名字是 "box2d"，就造 Box2D 物理世界；3D 的名字是 "jolt"，就造 Jolt 物理世界。如果底层没有编译进对应的引擎代码（没有定义对应的功能开关 GRYCE_HAS_BOX2D / GRYCE_HAS_JOLT），或者名字不认识，就记一条错误日志然后返回空，不让程序继续用不存在的世界。

---