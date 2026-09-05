#version 330 core

// 体积雾合成 shader
// 将 fog 体积纹理合成到场景颜色

in vec2 vUV;

out vec4 FragColor;

uniform sampler2D uSceneColor;
uniform sampler2D uFogTex;           // 2D 纹理模拟 3D volume（切片排列）
uniform sampler2D uDepthTex;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform vec2 uScreenSize;
uniform int uFogSliceCount;

// 从深度重建世界位置
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    vec3 scene_color = texture(uSceneColor, vUV).rgb;
    float depth = texture(uDepthTex, vUV).r;

    if (depth >= 1.0) {
        FragColor = vec4(scene_color, 1.0);
        return;
    }

    // 从深度重建世界位置，确定采样切片
    vec3 world_pos = world_from_depth(depth, vUV);
    float dist = length(world_pos - uCameraPos);

    // 计算切片索引
    float slice_idx = dist / uFogSliceCount;
    slice_idx = clamp(slice_idx, 0.0, float(uFogSliceCount - 1));

    // 在 fog 纹理中采样对应切片
    // fog 纹理布局：切片水平排列
    float slice_w = 1.0 / float(uFogSliceCount);
    vec2 fog_uv = vec2(vUV.x * slice_w + slice_idx * slice_w, vUV.y);
    vec4 fog_sample = texture(uFogTex, fog_uv);

    // 合成雾到场景颜色
    vec3 final_color = mix(scene_color, fog_sample.rgb, fog_sample.a);

    FragColor = vec4(final_color, 1.0);
}