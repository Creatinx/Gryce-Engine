#version 330 core

// 广告牌（Billboard）顶点着色器 —— 每张一条实例
//   binding 0（每顶点）：aCorner，静态单位四边形角标（-1..1），只上传一次
//   binding 1（每实例）：aCenter/aSize/aOpacity/aFlags，每张一份
//     aFlags.x = lock_x_axis（1 = 只绕世界 Y 偏航，保持竖直）
//     aFlags.y = shaded（1 = 用主方向光做廉价伪 Lambert 明暗）
// 朝向展开在这里完成，因此上传数据与相机无关。
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aCenter;
layout(location = 2) in vec2 aSize;
layout(location = 3) in float aOpacity;
layout(location = 4) in vec2 aFlags;

uniform mat4 uView;
uniform mat4 uProjection;

out vec2 vUV;
out vec4 vColor;

// 与场景主方向光一致的太阳方向（世界空间，指向光源），只用于 shaded 的廉价明暗。
const vec3 k_sun_dir = normalize(vec3(0.55, 0.70, -0.45));

void main() {
    // view 矩阵旋转部分的行向量就是世界空间的相机基向量
    // （列主序 mat4：uView[col][row]，行 0 = right，行 1 = up）
    vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
    vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);

    if (aFlags.x > 0.5) {
        // lock_x_axis：只绕世界 Y 偏航。把相机右向量压到水平面内、up 固定为世界 +Y，
        // 广告牌保持竖直（血条/公告不会随相机俯仰倾斜）。
        vec3 flat_right = vec3(right.x, 0.0, right.z);
        float flat_len = length(flat_right);
        // 相机正对上下时水平分量为零，退回固定右向量避免 NaN
        right = flat_len > 1e-6 ? flat_right / flat_len : vec3(1.0, 0.0, 0.0);
        up = vec3(0.0, 1.0, 0.0);
    }

    vec3 world = aCenter
               + right * (aCorner.x * aSize.x * 0.5)
               + up    * (aCorner.y * aSize.y * 0.5);

    // 伪 Lambert：法线取两个基向量的叉乘，abs 让背面同样受光（不剔除，两面一致）。
    vec3 normal = normalize(cross(right, up));
    float shade = mix(1.0, 0.65 + 0.35 * abs(dot(normal, k_sun_dir)), aFlags.y);

    vUV = aCorner * 0.5 + 0.5;
    vColor = vec4(vec3(shade), aOpacity);
    gl_Position = uProjection * uView * vec4(world, 1.0);
}