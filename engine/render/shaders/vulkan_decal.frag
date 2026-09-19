#version 450 core

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 FragColor;

// 特效 pass 参数：逆 VP、屏幕尺寸、贴花属性和变换
layout(set = 0, binding = 20, std140) uniform PassParamsUBO {
    layout(offset = 0) vec4 esm_param;
    layout(offset = 16) vec4 point_params;
    layout(offset = 32) vec4 point_light_pos;
    layout(offset = 48) vec4 atlas_offset;
    layout(offset = 64) vec4 screen_size;
    layout(offset = 80) vec4 decal_albedo;
    layout(offset = 96) mat4 decal_inv_view_proj;
    layout(offset = 160) mat4 decal_world_to_decal;
} pass;

// 标准描述符布局内、可用于深度重建的深度贴图 binding（Vulkan 下 decal 的
// 深度贴图 slot 超出 k_max_texture_bindings，无法单独绑定，回退到采样器兜底贴图）
layout(set = 0, binding = 15) uniform sampler2D uDepthTexture;

// 从深度重建世界位置
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = pass.decal_inv_view_proj * ndc;
    return world.xyz / max(world.w, 1e-6);
}

void main() {
    vec2 uv = gl_FragCoord.xy / max(pass.screen_size.xy, vec2(1.0));

    float depth = texture(uDepthTexture, uv).r;
    if (depth >= 1.0) discard;

    vec3 world_pos = world_from_depth(depth, uv);

    vec4 local_pos = pass.decal_world_to_decal * vec4(world_pos, 1.0);
    vec3 local = local_pos.xyz / max(local_pos.w, 1e-6);

    // 检查是否在贴花 AABB 内（[-0.5, 0.5] 范围）
    if (abs(local.x) > 0.5 || abs(local.y) > 0.5 || abs(local.z) > 0.5) discard;

    // 贴花 UV：投影到 XY 平面（纹理化未启用，仅使用纯色）
    vec3 albedo = pass.decal_albedo.xyz;

    // 输出贴花颜色（alpha blending 负责合成到场景）
    FragColor = vec4(albedo, pass.decal_albedo.w);
}