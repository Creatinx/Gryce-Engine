#version 450 core

// 广告牌（Billboard）顶点着色器（Vulkan，实例化）
// 两条顶点流（与 vk_shader.cpp 的 billboard_ 分支一致）：
//   binding 0（每顶点，stride 8）：aCorner，静态单位四边形角标
//   binding 1（每实例，stride 32）：aCenter/aSize/aOpacity/aFlags
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aCenter;
layout(location = 2) in vec2 aSize;
layout(location = 3) in float aOpacity;
layout(location = 4) in vec2 aFlags;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

// 与 C++ VulkanShader 非后处理管线的 push constant（4 个 mat4）对齐
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

// 与场景主方向光一致的太阳方向（世界空间，指向光源），只用于 shaded 的廉价明暗。
const vec3 k_sun_dir = normalize(vec3(0.55, 0.70, -0.45));

void main() {
    // view 矩阵旋转部分的行向量即世界空间相机基向量
    // （列主序 mat4：pc.uView[col][row]，行 0 = right，行 1 = up）
    vec3 right = vec3(pc.uView[0][0], pc.uView[1][0], pc.uView[2][0]);
    vec3 up    = vec3(pc.uView[0][1], pc.uView[1][1], pc.uView[2][1]);

    if (aFlags.x > 0.5) {
        // lock_x_axis：只绕世界 Y 偏航，广告牌保持竖直
        vec3 flat_right = vec3(right.x, 0.0, right.z);
        float flat_len = length(flat_right);
        right = flat_len > 1e-6 ? flat_right / flat_len : vec3(1.0, 0.0, 0.0);
        up = vec3(0.0, 1.0, 0.0);
    }

    vec3 world = aCenter
               + right * (aCorner.x * aSize.x * 0.5)
               + up    * (aCorner.y * aSize.y * 0.5);

    vec3 normal = normalize(cross(right, up));
    float shade = mix(1.0, 0.65 + 0.35 * abs(dot(normal, k_sun_dir)), aFlags.y);

    vUV = aCorner * 0.5 + 0.5;
    vColor = vec4(vec3(shade), aOpacity);
    gl_Position = pc.uProjection * pc.uView * vec4(world, 1.0);
}