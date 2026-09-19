#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;
uniform mat4 uModel;
uniform mat4 uLightSpaceMatrix;
// 双抛物面映射: face=0 (front), face=1 (back)
uniform int uParaboloidFace;
uniform float uPointLightRange;
uniform vec3 uPointLightPos;
out vec2 vTexCoord;
void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vec3 to_light = world_pos.xyz - uPointLightPos;
    float dist = length(to_light);
    vec3 dir = to_light / dist;
    // 双抛物面映射: front face (z>=0), back face (z<0)
    float z = (uParaboloidFace == 0) ? dir.z : -dir.z;
    float a = 1.0 / (1.0 + z);
    vec2 uv = dir.xy * a * 0.5 + 0.5;
    // 深度写入: 使用归一化距离
    float depth = dist / uPointLightRange;
    // 使用标准投影确保深度写入正确
    // 双抛物面映射: 把位置投影到近裁剪面
    vec4 proj = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    gl_Position = proj;
    vTexCoord = aTexCoord;
}