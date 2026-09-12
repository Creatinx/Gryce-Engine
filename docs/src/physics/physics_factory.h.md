# 文件名：physics_factory.h

## 文件作用

这个头文件打开了物理世界"工厂"的大门，声明了两个最顶层的创建函数：create_physics_world_2d 和 create_physics_world_3d。调用者只要给这两个函数传一个名字（比如 "box2d"、"jolt"），它就会按名字返回对应的 2D 或 3D 物理世界对象。真正怎么按名字造出对象，在这个头文件对应的 .cpp 文件里实现。

---