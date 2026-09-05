#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

out vec2 vTexCoord;

uniform mat4 uLightSpaceMatrix;
uniform mat4 uModel;
uniform float uNormalOffset;
uniform int uUseAlbedoMap;
uniform vec4 uAtlasOffset;  // xy=slot offset in atlas, zw=slot size (normalized)

void main() {
    // Normal Offset Shadow Mapping：沿法线把几何推向光源
    vec3 world_pos = (uModel * vec4(aPos, 1.0)).xyz;
    vec3 N = normalize(mat3(uModel) * aNormal);
    world_pos += N * uNormalOffset;
    gl_Position = uLightSpaceMatrix * vec4(world_pos, 1.0);

    // 将坐标映射到 atlas slot 区域
    // 先把 NDC [-1,1] 映射到 [0,1], 然后偏移到 slot 位置, 再映射回 [-1,1]
    vec2 slot_uv = gl_Position.xy / gl_Position.w;           // [-1, 1]
    slot_uv = slot_uv * 0.5 + 0.5;                            // [0, 1]
    slot_uv = slot_uv * uAtlasOffset.zw + uAtlasOffset.xy;   // 映射到 slot 区域
    slot_uv = slot_uv * 2.0 - 1.0;                            // 回 [-1, 1]
    gl_Position.xy = slot_uv * gl_Position.w;

    // [Alpha Test] 传递 UV 坐标供 frag 阶段采样 alpha 通道
    vTexCoord = aTexCoord;
}