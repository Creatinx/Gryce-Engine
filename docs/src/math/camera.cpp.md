# 文件名：camera.cpp

## 文件作用

这个文件是"摄像机"的具体实现。摄像机就像游戏里的一双眼睛，负责记录玩家的位置、看向哪里，并算出"从哪个角度看世界"要用的矩阵。

## 全局内容：文件开头的引入和命名空间

（第1行）`#include "camera.h"`
→ 解释：把 camera.h 这个头文件里的声明（比如 Camera 类长什么样、有哪些函数）拿进来，这样下面才能用到 Camera。

（第2行）空行
→ 解释：没有实际内容，只是分隔一下版面。

（第3行）`#include <cmath>`
→ 解释：把系统自带的数学库 cmath 拿进来，里面提供了 cos（余弦）、sin（正弦）、atan2 这些数学函数，后面会用。

（第4行）空行
→ 解释：分隔版面用的空行。

（第5行）`namespace gryce_engine::math {`
→ 解释：进入一个叫 gryce_engine::math 的"名字空间"，作用是给里面的东西起个唯一的名字，避免和别的代码重名，表示这些都是数学相关的内容。

（第6行）空行
→ 解释：分隔版面用的空行。

## 函数名：Camera 构造函数
位置：camera.cpp 第 7-11 行
作用：创建一个摄像机对象时，把它的初始位置、朝向、右手方向、上方方向等先设定好。
逐行解析：

（第7行）`Camera::Camera()`
→ 解释：定义一个函数，名字是 Camera::Camera()，前面的 Camera 表示它是 Camera 这个类的成员函数，后面那个 Camera() 表示它是构造函数——就是创建摄像机时自动运行的函数。

（第8行）`    : position_(0.0f, 0.0f, 5.0f)`
→ 解释：把位置 position_ 设定为 x=0、y=0、z=5，意思是摄像机最开始站在坐标 (0,0,5) 这个点上。0.0f 和 5.0f 里的 f 表示这是小数（浮点数）。

（第9行）`    , forward_(0.0f, 0.0f, -1.0f)`
→ 解释：把前方向 forward_ 设定为 (0,0,-1)，表示摄像机一开始面朝 z 轴负方向看。

（第10行）`    , right_(1.0f, 0.0f, 0.0f)`
→ 解释：把右方向 right_ 设定为 (1,0,0)，表示摄像机的正右边指向 x 轴正方向。

（第11行）`    , up_(0.0f, 1.0f, 0.0f) {}`
→ 解释：把上方向 up_ 设定为 (0,1,0)，表示摄像机的正上方指向 y 轴正方向。最后的 {} 表示构造函数的内容到这里结束，没有别的代码了。

数据流：输入是固定的默认值（位置和几个方向向量），没有外部输入；这些值直接被存进摄像机的成员变量里。构造函数不调用别的函数。
调用关系：谁调用它——在代码里创建 Camera 对象时自动调用（比如声明 `Camera cam;`）。它调用谁——不调用其他函数。

## 函数名：Camera::update
位置：camera.cpp 第 13-38 行
作用：每一帧（每刷新一次画面）都调用它，根据按键和鼠标移动来更新摄像机的位置和视线方向。
逐行解析：

（第13行）`void Camera::update(float delta_time,`
→ 解释：定义一个不返回内容的函数（void 就是没有返回值），名字是 Camera::update，它接收的第一个参考量叫 delta_time，代表从上一帧到这一帧经过的时间。

（第14行）`                    bool move_forward, bool move_backward,`
→ 解释：接着传进来的两个开关 move_forward（是否往前走）和 move_backward（是否往后走），都是"是/否"这种真假值。

（第15行）`                    bool move_left, bool move_right,`
→ 解释：接着传进来的两个开关 move_left（是否往左走）和 move_right（是否往右走）。

（第16行）`                    bool move_up, bool move_down,`
→ 解释：接着传进来的两个开关 move_up（是否往上走）和 move_down（是否往下走）。

（第17行）`                    bool sprint,`
→ 解释：接着传进来的开关 sprint（是否在冲刺加速跑）。

（第18行）`                    float mouse_delta_x, float mouse_delta_y) {`
→ 解释：最后传进来的两个小数 mouse_delta_x（鼠标横向移动的量）和 mouse_delta_y（鼠标纵向移动的量）。整个参数表用右括号 ) 结束，后面跟 { 表示函数真正的代码块从这里开始。

（第19行）`    float speed = move_speed_ * (sprint ? sprint_multiplier_ : 1.0f);`
→ 解释：算出移动速度。意思是：如果正在冲刺（sprint 是真的），速度就是原来的移动速度 move_speed_ 乘以冲刺倍率 sprint_multiplier_；否则（sprint 是假的）就只是普通速度 move_speed_ 乘以 1.0f（也就是不变）。

（第20行）空行
→ 解释：分隔版面用的空行。

（第21行）`    Vector3f move(0.0f, 0.0f, 0.0f);`
→ 解释：创建一个向量 move，把它的 x、y、z 都设为 0，用来暂时汇总这一次要往哪个方向移动。

（第22行）`    if (move_forward)  move += forward_;`
→ 解释：如果按了往前走（move_forward 是真的），就把前方向 forward_ 加进 move 里，表示要往前移动。

（第23行）`    if (move_backward) move -= forward_;`
→ 解释：如果按了往后走，就把前方向 forward_ 从 move 里减掉，表示要往反方向（后面）移动。

（第24行）`    if (move_left)     move -= right_;`
→ 解释：如果按了往左走，就把右方向 right_ 从 move 里减掉，表示要往左移动。

（第25行）`    if (move_right)    move += right_;`
→ 解释：如果按了往右走，就把右方向 right_ 加进 move 里，表示要往右移动。

（第26行）`    if (move_up)       move += Vector3f::up();`
→ 解释：如果按了往上升，就把世界的正上方方向（Vector3f::up() 返回固定方向 (0,1,0)）加进 move 里，表示要往上移动。

（第27行）`    if (move_down)     move -= Vector3f::up();`
→ 解释：如果按了往下降，就把世界的正上方方向从 move 里减掉，表示要往下移动。

（第28行）空行
→ 解释：分隔版面用的空行。

（第29行）`    if (move.length_sq() > 0.0f) {`
→ 解释：判断 move 这个向量的长度平方是否大于 0，也就是判断有没有按任何移动键。length_sq() 是算"长度平方"的函数，用平方来比大小可以省去开方运算、更快。如果大于 0 就说明确实要移动。

（第30行）`        position_ += move.normalized() * speed * delta_time;`
→ 解释：把位置 position_ 加上"移动方向 × 速度 × 时间"。normalized() 会把 move 变成长度是 1 的单位向量（只留方向），这样不用担心斜着走更快。乘以 speed（速度）和时间 delta_time 就得到这一帧实际该走的距离。

（第31行）`    }`
→ 解释：这个右括号表示上面那个 if（第29行开始的判断）到这里结束。

（第32行）空行
→ 解释：分隔版面用的空行。

（第33行）`    yaw_   += mouse_delta_x * mouse_sensitivity_;`
→ 解释：把摄像机的左右转头角度 yaw_ 加上"鼠标横向移动量 × 鼠标灵敏度"。意思是鼠标往横向移，摄像机就左右转。

（第34行）`    pitch_ += mouse_delta_y * mouse_sensitivity_;`
→ 解释：把摄像机的上下俯仰角度 pitch_ 加上"鼠标纵向移动量 × 鼠标灵敏度"。意思是鼠标往纵向移，摄像机就上下看。

（第35行）`    pitch_ = math::clamp(pitch_, -89.0f, 89.0f);`
→ 解释：把上下的角度 pitch_ 限制在 -89 到 89 度之间，防止角度太夸张导致画面倒过来。clamp 就是"夹住，不让超出范围"的意思，这里用的是 math 名字空间里的函数。

（第36行）空行
→ 解释：分隔版面用的空行。

（第37行）`    update_vectors();`
→ 解释：调用本类里另一个函数 update_vectors()，重新根据新的角度算出摄像机的前、右、上三个方向向量。

（第38行）`}`
→ 解释：这个右括号表示 update 这个函数到这里结束。

数据流：输入是 delta_time（时间）、一堆按键开关（布尔值）和鼠标移动量（两个小数）；经过函数里的计算（先算速度、再汇总移动方向、再改位置、再改角度）后输出结果是位置、角度、方向向量的更新（都写在摄像机的成员变量里）。
调用关系：谁调用它——在游戏主循环里每帧调用。它调用谁——调用了 update_vectors()（重算方向）和 math::clamp（夹角度）、Vector3f 的 normalized / length_sq / up 等。

## 函数名：Camera::update_vectors
位置：camera.cpp 第 40-51 行
作用：根据当前的左右角度和上下角度，重新算出摄像机的前、右、上三个方向向量。
逐行解析：

（第40行）`void Camera::update_vectors() {`
→ 解释：定义一个不返回内容的函数 update_vectors()，开头写改名符 void 表示没有返回值，左括号 { 表示函数体从这里开始。

（第41行）`    float yaw_rad   = math::to_radians(yaw_);`
→ 解释：把左右角度 yaw_ 从"度"换算成"弧度"，存到变量 yaw_rad 里。因为数学里的三角函数要用弧度，to_radians 就是"度转弧度"的函数。

（第42行）`    float pitch_rad = math::to_radians(pitch_);`
→ 解释：把上下角度 pitch_ 从"度"换算成"弧度"，存到变量 pitch_rad 里，方便下面的三角函数使用。

（第43行）空行
→ 解释：分隔版面用的空行。

（第44行）`    forward_.x = std::cos(yaw_rad) * std::cos(pitch_rad);`
→ 解释：用三角函数算出前方向向量的 x 分量。cos 是余弦函数，std:: 表示用系统数学库里的这个函数。前后方向随夹角余弦变化。

（第45行）`    forward_.y = std::sin(pitch_rad);`
→ 解释：算出前方向向量的 y 分量（上下分量），y = 仰角的正弦值。sin 是正弦函数。

（第46行）`    forward_.z = std::sin(yaw_rad) * std::cos(pitch_rad);`
→ 解释：算出前方向向量的 z 分量，等于左右角度的正弦乘以上下角度的余弦。

（第47行）`    forward_ = forward_.normalized();`
→ 解释：把刚算出来的前方向 forward_ 变成长度为 1 的单位向量（只保留方向），这样后面算距离和交叉积才不会出错。

（第48行）空行
→ 解释：分隔版面用的空行。

（第49行）`    right_ = forward_.cross(Vector3f::up()).normalized();`
→ 解释：算右方向。用前方向 forward_ 和世界正上方做"交叉积"（cross 是叉乘），得到同时垂直于两者的方向，再转成单位向量，这个方向就是摄像机的正右方。

（第50行）`    up_    = right_.cross(forward_).normalized();`
→ 解释：算上方向。用刚算出的右方向 right_ 和前方向 forward_ 做交叉积，得到摄像机的真正上方方向，再转成单位向量。

（第51行）`}`
→ 解释：这个右括号表示 update_vectors 函数到这里结束。

数据流：输入是成员变量 yaw_ 和 pitch_（两个角度）；经过程度转弧度、三角函数、交叉积、归一化后输出前、右、上三个方向向量，并写回成员变量。
调用关系：谁调用它——update() 和 look_at() 都会调用它。它调用谁——调用 math::to_radians、std::cos、std::sin、Vector3f 的 normalized 和 cross。

## 函数名：Camera::look_at
位置：camera.cpp 第 53-58 行
作用：让摄像机直接盯着某个目标点看，自动算出合适的上下角度和左右角度。
逐行解析：

（第53行）`void Camera::look_at(const Vector3f& target) {`
→ 解释：定义一个不返回内容的函数 look_at，它接收一个参考参数 target（目标点）。前面加 const 表示只是"借用"这个参考不修改它；& 表示传进来的是参考（不用重新拷贝一份，更省事）。

（第54行）`    Vector3f dir = (target - position_).normalized();`
→ 解释：算出由摄像机指向目标的方向。先把目标点 target 减掉摄像机位置 position_，得到"摄像机到目标"的连线向量，再转成单位向量，取名 dir。

（第55行）`    pitch_ = math::to_degrees(std::asin(math::clamp(dir.y, -1.0f, 1.0f)));`
→ 解释：算上下角度。把方向向量的 y 分量夹在 -1 到 1 之间，再用反正弦函数 asin 算出角度（弧度），最后用 to_degrees 转成"度"，存到 pitch_ 里。

（第56行）`    pitch_ = math::to_degrees(std::atan2(dir.z, dir.x));`
→ 解释：算左右角度。用 atan2 函数根据方向的 z 和 x 分量算出左右转角（弧度），再用 to_degrees 转成"度"。注意这里其实想写的是 yaw_，因为左右角度应该存进 yaw_，暂时看不懂，但它大概是想把左右角度保存下来。

（第57行）`    update_vectors();`
→ 解释：调用 update_vectors()，根据刚算好的角度重新更新前、右、上三个方向向量，让摄像机真正朝目标看去。

（第58行）`}`
→ 解释：这个右括号表示 look_at 函数到这里结束。

数据流：输入是目标点 target；经过目标减当前位置得出方向、反正弦反arctan算角度、更新方向向量后输出是更新好的 pitch_、yaw_ 和方向向量。
调用关系：谁调用它——外部想让摄像机盯住某目标时调用。它调用谁——调用 math::to_degrees、std::asin、math::clamp、std::atan2、update_vectors()。

## 函数名：Camera::get_view_matrix
位置：camera.cpp 第 60-62 行
作用：算出一个"观察矩阵"，这个矩阵用来把世界坐标转成摄像机自己看到的坐标。
逐行解析：

（第60行）`Matrix4f Camera::get_view_matrix() const {`
→ 解释：定义一个返回 Matrix4f（4 乘 4 矩阵）的函数 get_view_matrix。最后一个 const 表示这个函数不会修改摄像机自己的数据，只负责读取并计算。

（第61行）`    return Matrix4f::look_at(position_, position_ + forward_, up_);`
→ 解释：返回用 Matrix4f 类里的静态函数 look_at 算出的矩阵。look_at 需要三个点/向量：摄像机位置 position_、摄像机看向的点（位置加上前方向）、以及上方向 up_。看起点和看止点定好视线，用 up_ 确定竖的方向，就能生成观察矩阵。

（第62行）`}`
→ 解释：这个右括号表示 get_view_matrix 函数到这里结束。

数据流：输入是成员变量 position_、forward_、up_；直接交给 Matrix4f::look_at 计算，输出一个观察矩阵并返回。
调用关系：谁调用它——渲染画面、计算光的变换时会调用。它调用谁——调用 Matrix4f::look_at。

## 函数名：Camera::get_projection_matrix（第一个版本）
位置：camera.cpp 第 64-66 行
作用：算出一个"投影矩阵"，这个矩阵让近处的东西看起来大、远处的东西看起来小，形成立体感。这个版本需要一个 aspect 参数。
逐行解析：

（第64行）`Matrix4f Camera::get_projection_matrix(float aspect) const {`
→ 解释：定义一个返回 Matrix4f 的函数 get_projection_matrix，它接收一个小数 aspect（屏幕宽高比），const 表示不改动摄像机数据。

（第65行）`    return Matrix4f::perspective(math::to_radians(fov_), aspect, near_, far_);`
→ 解释：调用 Matrix4f 类的静态函数 perspective（透视投影）来生成矩阵，需要传四个值：把视野角度 fov_ 由度转弧度、宽高比 aspect、近裁剪距离 near_、远裁剪距离 far_。

（第66行）`}`
→ 解释：这个右括号表示这个 get_projection_matrix 函数到这里结束。

数据流：输入是外部传入的 aspect 加上成员变量 fov_、near_、far_；交给 Matrix4f::perspective 计算，输出投影矩阵并返回。
调用关系：谁调用它——需要屏幕上物体有远近效果时调用。它调用谁——调用 math::to_radians 和 Matrix4f::perspective。

## 函数名：Camera::get_projection_matrix（第二个版本）
位置：camera.cpp 第 68-70 行
作用：跟上一个函数功能一样，也是算投影矩阵，只不过这个版本不带参数，直接使用摄像机自己存好的宽高比 aspect_。
逐行解析：

（第68行）`Matrix4f Camera::get_projection_matrix() const {`
→ 解释：定义另一个同名的 get_projection_matrix 函数，但这个版本括号里没有参数，直接使用成员变量里的宽高比。C 语言允许同名函数，只要参数不一样就能区分，这种写法叫"函数重载"。

（第69行）`    return Matrix4f::perspective(math::to_radians(fov_), aspect_, near_, far_);`
→ 解释：跟上一个函数一样调用 perspective，区别是这里直接使用摄像机自己存的宽高比 aspect_，而不是外部传进来的。

（第70行）`}`
→ 解释：这个右括号表示这第二个 get_projection_matrix 函数到这里结束。

数据流：输入是成员变量 fov_、aspect_、near_、far_；交给 Matrix4f::perspective 计算，输出投影矩阵并返回。
调用关系：谁调用它——调用者不想自己提供宽高比时用这个版本。它调用谁——调用 math::to_radians 和 Matrix4f::perspective。

## 全局内容：命名空间结束

（第71行）空行
→ 解释：分隔版面用的空行。

（第72行）`} // namespace gryce_engine::math`
→ 解释：这个右括号表示最外层那个名字空间 gryce_engine::math（第5行开的）到这里结束，后面的注释说明了这一点。整个文件到这里就结束了。