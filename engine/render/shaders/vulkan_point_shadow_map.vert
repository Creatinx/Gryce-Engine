#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;

// 颜色 pass：C++ 推送 { model, view, projection, lightspace } 4 个 mat4（VulkanShader::push_constants else 分支）
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

// 特效 pass 参数：point_params.x=range, y=paraboloid_face；point_light_pos 位看向量
layout(set = 0, binding = 20, std140) uniform PassParamsUBO {
    layout(offset = 0) vec4 esm_param;
    layout(offset = 16) vec4 point_params;
    layout(offset = 32) vec4 point_light_pos;
    layout(offset = 48) vec4 atlas_offset;
    layout(offset = 64) vec4 screen_size;
    layout(offset = 80) vec4 decal_albedo;
    layout(offset = 96) mat4 mat_extra;
} pass;

layout(location = 0) out vec2 vTexCoord;

void main() {
    vec4 world_pos = pc.uModel * vec4(aPos, 1.0);
    vec3 to_light = world_pos.xyz - pass.point_light_pos.xyz;
    float dist = length(to_light);
    vec3 dir = to_light / max(dist, 1e-6);

    // 双抛物面映射：front face (z>=0), back face (z<0)
    float z = (pass.point_params.y < 0.5) ? dir.z : -dir.z;
    float a = 1.0 / (1.0 + z);
    vec2 uv = dir.xy * a * 0.5 + 0.5;

    // 深度写入：归一化距离
    float depth = dist / max(pass.point_params.x, 1e-6);
    vec4 proj = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    gl_Position = proj;
    vTexCoord = aTexCoord;
}