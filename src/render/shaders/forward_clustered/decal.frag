#version 330 core

// 贴花投影 shader
// 渲染贴花 AABB 几何体，从深度重建世界位置，投影贴花纹理
// 使用 alpha blending 合成到场景颜色

out vec4 FragColor;

uniform sampler2D uDepthTex;
uniform sampler2D uAlbedoTex;
uniform mat4 uInvViewProj;
uniform mat4 uWorldToDecal;    // 世界到贴花局部矩阵
uniform vec3 uDecalAlbedo;
uniform float uDecalOpacity;
uniform vec2 uScreenSize;
uniform int uUseAlbedoTex;

// 从深度重建世界位置
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    vec2 uv = gl_FragCoord.xy / uScreenSize;

    // 读取深度
    float depth = texture(uDepthTex, uv).r;
    if (depth >= 1.0) {
        discard;
        return;
    }

    // 重建世界位置
    vec3 world_pos = world_from_depth(depth, uv);

    // 转换到贴花局部空间
    vec4 local_pos = uWorldToDecal * vec4(world_pos, 1.0);
    vec3 local = local_pos.xyz / local_pos.w;

    // 检查是否在贴花 AABB 内（[-0.5, 0.5] 范围）
    if (abs(local.x) > 0.5 || abs(local.y) > 0.5 || abs(local.z) > 0.5) {
        discard;
        return;
    }

    // 贴花 UV：投影到 XY 平面
    vec2 decal_uv = local.xy + 0.5;

    // 采样贴花纹理
    vec3 albedo = uDecalAlbedo;
    if (uUseAlbedoTex != 0) {
        albedo *= texture(uAlbedoTex, decal_uv).rgb;
    }

    // 输出贴花颜色（alpha blending 负责合成到场景）
    FragColor = vec4(albedo, uDecalOpacity);
}