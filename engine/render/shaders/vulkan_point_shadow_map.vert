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

    // 深度写入：归一化距离。Vulkan 的 NDC 深度区间是 [0,1]（不是 GL 的 [-1,1]），
    // 直接把 depth*2-1 塞进 gl_Position.z 会被裁掉一半（depth<0.5 的几何全部
    // 落在 z<0 的裁剪体之外），表现为"阴影图里只有远处、近处物体不投影"。
    float depth = dist / max(pass.point_params.x, 1e-6);
    // Y 必须取反：Vulkan 后端用「负高度视口」模拟 GL 的 Y 向上 NDC（见
    // VulkanBackend::set_viewport），于是 NDC y=-1 落在帧缓冲最后一行、纹理 v=1，
    // 而片元阶段按 dir 算出的 uv.y=0 读的是 v=0 —— 不翻转的话整张双抛物面图
    // 上下颠倒，遮挡体与采样位置错开，阴影表现为"哪里都没有"。
    vec4 proj = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    gl_Position = proj;
    vTexCoord = aTexCoord;
}