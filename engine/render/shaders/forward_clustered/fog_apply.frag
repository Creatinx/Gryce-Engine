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
uniform vec2 uFogRange;              // x=近, y=远（与 fog.frag 的切片区间一致）
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

    // 计算切片索引。fog.frag 的切片 i 覆盖距离 [(i/N)*far, ((i+1)/N)*far]，
    // 因此这里必须把视图距离按 far 归一化后再乘切片数；直接用 dist/N 会让索引
    // 恒等于 0（dist << far），只采样到近平面附近那一层空雾。
    float slice_f = clamp(dist / max(uFogRange.y, 0.0001), 0.0, 1.0) * float(uFogSliceCount);
    float slice_idx = clamp(floor(slice_f), 0.0, float(uFogSliceCount - 1));

    // 在 fog 纹理中采样对应切片
    // fog 纹理布局：切片水平排列
    float slice_w = 1.0 / float(uFogSliceCount);
    vec2 fog_uv = vec2(vUV.x * slice_w + slice_idx * slice_w, vUV.y);
    vec4 fog_sample = texture(uFogTex, fog_uv);

    // 合成雾到场景颜色。fog.frag 输出的是「预乘」结果：
    // rgb = 累积散射色，a = 1 - 透射率。因此正确的合成是
    //   scene * (1 - a) + rgb
    // 而不是 mix(scene, rgb, a)（后者会在浓雾时把画面压黑而不是染上雾色）。
    vec3 final_color = scene_color * (1.0 - fog_sample.a) + fog_sample.rgb;

    FragColor = vec4(final_color, 1.0);
}