# 文件名：world.h

## 文件作用

这个文件是干什么的：声明了 ECS 的 World 类的接口，World 负责把一个场景和一组系统放在一起统一管理。它提供挂载/卸载场景、注册系统、按名字或类型查找系统的方法。还负责启动（init）、关闭（shutdown），以及每一帧按阶段和优先级去驱动所有系统执行更新（update）和渲染（render）。

主要接口有：attach_scene / detach_scene（管理场景）、register_system / add_system / get_system（管理系统）、init / shutdown（生命周期）、update / render（每帧驱动）。World 用一张表按名字查系统，并能整体暂停或恢复系统的更新。

---