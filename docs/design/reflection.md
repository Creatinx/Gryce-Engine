# 组件反射系统（Reflection）设计

> 状态：M1-E1 已实现。Inspector 自动生成的前置；scene_serializer 收敛到反射是后续工作（本轮不改 serializer）。

## 1. 目标与边界

- 编辑器（及任意工具代码）能通过类型名枚举任意已注册组件的字段：名称、类型 tag、只读标记、可选 min/max/显示名。
- 通过类型擦除 API 读写字段值，无需为每个组件手写 UI 或序列化代码。
- 不引入外部依赖（不用 RTTI 库 / 代码生成器 / 第三方反射库），纯 C++17 宏 + 模板。

非目标（本轮不做）：嵌套结构、容器（vector/map）、enum 下拉、函数/方法反射、代码生成。

## 2. 注册机制选型

| 方案 | 结论 |
|---|---|
| 编译期宏注册（静态初始化 + TypeBuilder 链式） | **采用**。主流自研引擎做法（UE UPROPERTY / 自研宏的轻量版），零依赖、调试友好、字段增删只改一处 |
| 纯模板自动推导 | 否决：无法给字段起名字、无法标记只读/范围，仍需宏补充 |
| 代码生成（解析头文件生成注册代码） | 否决：引入构建期工具链，成本远超收益 |
| RTTI dynamic_cast 树 | 否决：只解决类型识别，不解决字段枚举 |

注册代码集中放在 `core/reflection/builtin_reflections.cpp`（本轮），与 `component_factory.cpp` 的集中注册风格一致；组件头/实现文件零改动。后续若组件字段频繁变动，可把注册块下沉到各组件 .cpp，机制不变。

## 3. 核心 API（core/reflection/reflection.h，namespace gryce_engine::reflection）

### 3.1 字段元信息

```cpp
enum class FieldType {
    Int, Float, Double, Bool, String,
    Vector2f, Vector3f, Vector4f, Quaternionf
};

struct FieldInfo {
    std::string name;           // 字段名（与 C++ 成员一致）
    std::string display_name;   // Inspector 显示名（默认 = name）
    FieldType type;
    bool read_only = false;
    bool has_range = false;
    float range_min = 0, range_max = 0;   // Inspector 滑条范围（仅标量）
    // 类型擦除读写：obj 为组件对象地址，dst/src 为对应 C++ 类型地址
    std::function<void(const void* obj, void* dst)> read;
    std::function<bool(void* obj, const void* src)> write;  // read_only 返回 false
};
```

读写闭包在注册时由成员指针生成，不存 offset——组件类含虚函数，非 standard-layout，`offsetof` 不合法；成员指针 + lambda 访问是唯一可移植路径。闭包对继承字段同样安全（编译器按声明类解析基类成员）。

### 3.2 类型注册表

```cpp
struct TypeInfo {
    std::string name;
    std::string parent_name;        // 继承链（如 "SkinnedMeshRenderer" → "Component"）
    std::vector<FieldInfo> fields;  // 仅本类声明的字段
};

class Registry {
    static Registry& instance();
    const TypeInfo* find(const std::string& name) const;
    // 继承链展开：基类（Component::enabled）在前，派生类字段在后
    std::vector<const FieldInfo*> all_fields(const std::string& name) const;
};
```

单例用函数内 static（construct-on-first-use），跨 TU 静态初始化顺序安全。

### 3.3 宏接口（一行式）

```cpp
GRYCE_REFLECT_CLASS(Transform, Component)
    GRYCE_REFLECT_FIELD(position)
    GRYCE_REFLECT_FIELD(rotation)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RO(some_computed_field)
GRYCE_REFLECT_END()
```

- `GRYCE_REFLECT_CLASS(Class, Base)`：展开为匿名命名空间内的 `TypeBuilder<Class>` 静态初始化链起点。
- `GRYCE_REFLECT_FIELD(field)`：成员指针类型自动推导字段类型 tag（`field_type_of<T>()` 模板特化；未映射类型 static_assert 报错，编译期阻止误注册）。
- `_RANGE` 变体：标量 min/max；`_RO` 变体：只读（write 恒返回 false）。

### 3.4 读写访问

```cpp
template<typename T> T    read_field(const void* obj, const FieldInfo& f);
template<typename T> bool write_field(void* obj, const FieldInfo& f, const T& value);
// field_type_of<T>() → FieldType，调用方可先比对 f.type 保证类型安全
```

## 4. 与既有系统的关系

| 模块 | 关系 |
|---|---|
| `Component` / `ComponentFactory` | 正交：Factory 解决「按类型名创建组件」，反射解决「按类型名枚举/读写字段」。`Component::type()` 字符串即反射类型名，一一对应 |
| `Component::enabled` | 注册为 `Component` 类型的唯一字段，所有组件经继承链可见 |
| `scene_serializer` | 本轮不改。后续可用手写 serialize 逐步替换为「遍历 all_fields + FieldType 分派」；资源路径类字段（string）已可直接覆盖 |
| Inspector（M1-E2） | `all_fields(type)` + FieldType → 自动生成控件：Bool→checkbox、标量→drag/slider（has_range）、Vec3→3 float、String→text、read_only→禁用态 |

## 5. 已注册组件（首批 11 个 + 基类）

Component（enabled）、Transform、MeshRenderer、SkinnedMeshRenderer、Camera、Light、Sprite2D（width/height 等标量字段）、AudioSource、RigidBody、BoxCollider、CharacterController3D。

注册原则：只注册**编辑器有意义的公有值字段**；跳过 GPU 句柄、unique_ptr<Material>、alive_token 等运行时/非序列化字段；跳过 enum（本轮 FieldType 无 enum，后续补）。

## 6. 已知限制 / 后续

- enum 字段（Light::light_type 等）未注册 → 后续加 FieldType::Enum + 选项表。
- 嵌套结构（Material）、容器、资源句柄富类型 → M2 按需补。
- scene_serializer 收敛到反射 → 独立任务，需处理既有存档兼容。
