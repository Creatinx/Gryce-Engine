#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outESM;

// 材质 UBO：alpha test 所需字段
layout(set = 0, binding = 0, std140) uniform MaterialUBO {
    layout(offset = 32) vec4 uEmissiveOpacity;
    layout(offset = 96) int uUseAlbedoMap;
} ubo;

// 特效 pass 共享参数 UBO（binding 20）：ESM 指数在 esm_param.x
layout(set = 0, binding = 20, std140) uniform PassParamsUBO {
    layout(offset = 0) vec4 esm_param;
    layout(offset = 16) vec4 point_params;
    layout(offset = 32) vec4 point_light_pos;
    layout(offset = 48) vec4 atlas_offset;
    layout(offset = 64) vec4 screen_size;
    layout(offset = 80) vec4 decal_albedo;
    layout(offset = 96) mat4 mat_extra;
} pass;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;

void main() {
    float depth = gl_FragCoord.z;
    float exponent = max(pass.esm_param.x, 1e-4);
    outESM = vec4(exp(-exponent * depth), 0.0, 0.0, 1.0);

    // Alpha Test
    if (ubo.uUseAlbedoMap > 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a * ubo.uEmissiveOpacity.w;
        if (alpha < 0.5) discard;
    }
}