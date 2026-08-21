#version 330 core

// 体积雾渲染 shader
// 从深度缓冲重建世界位置，逐体素计算雾密度
// 输出到 3D fog 体积纹理（切片存储为 2D 纹理）

in vec2 vUV;

out vec4 FogColor;

uniform sampler2D uDepthTex;
uniform sampler2D uNormalTex;       // 法线+粗糙度（用于雾遮挡）
uniform mat4 uInvViewProj;
uniform mat4 uViewMatrix;
uniform vec3 uCameraPos;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uFogHeight;           // 雾高度（从地面起算）
uniform vec2 uFogRange;             // x=近, y=远
uniform vec2 uScreenSize;           // 视口尺寸
uniform int uFogSliceIndex;         // 当前切片索引（Z 方向）
uniform int uFogSliceCount;         // 切片总数

// 从深度重建世界位置
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// 计算雾密度
float compute_fog_density(vec3 world_pos, float step_size) {
    float height = world_pos.y;
    // 基于高度的雾密度（指数衰减）
    float height_factor = exp(-abs(height) / max(uFogHeight, 0.01));
    // 距离衰减
    float dist = length(world_pos - uCameraPos);
    float dist_factor = clamp((dist - uFogRange.x) / (uFogRange.y - uFogRange.x), 0.0, 1.0);
    // 最终密度
    return uFogDensity * height_factor * dist_factor * step_size;
}

void main() {
    // 读取深度
    float depth = texture(uDepthTex, vUV).r;
    if (depth >= 1.0) {
        FogColor = vec4(0.0);
        return;
    }

    // 重建世界位置
    vec3 world_pos = world_from_depth(depth, vUV);

    // 计算从相机到表面的光线方向
    vec3 view_dir = normalize(world_pos - uCameraPos);
    float view_dist = length(world_pos - uCameraPos);

    // 切片信息
    float slice_count = float(uFogSliceCount);
    float slice_idx = float(uFogSliceIndex);
    float slice_start = (slice_idx / slice_count) * uFogRange.y;
    float slice_end = ((slice_idx + 1.0) / slice_count) * uFogRange.y;

    // 限制在当前切片范围内
    float start_t = max(0.0, (slice_start - uFogRange.x) / max(view_dist - uFogRange.x, 0.001));
    float end_t = min(1.0, (slice_end - uFogRange.x) / max(view_dist - uFogRange.x, 0.001));

    // 光线步进累积雾密度
    const int k_steps = 8;
    float step_size = (end_t - start_t) / float(k_steps);
    vec3 accum_color = vec3(0.0);
    float accum_density = 0.0;
    float transmittance = 1.0;

    for (int i = 0; i < k_steps; ++i) {
        float t = start_t + (float(i) + 0.5) * step_size;
        vec3 sample_pos = uCameraPos + view_dir * (uFogRange.x + t * (view_dist - uFogRange.x));
        float density = compute_fog_density(sample_pos, step_size * view_dist);
        if (density > 0.0) {
            float sample_trans = exp(-density);
            accum_color += uFogColor * density * transmittance * step_size * view_dist;
            transmittance *= sample_trans;
            accum_density += density;
        }
    }

    FogColor = vec4(accum_color, 1.0 - transmittance);
}