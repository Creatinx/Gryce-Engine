#version 450 core

layout(location = 0) in vec3 vDir;

layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform samplerCube uSkybox;

void main() {
    FragColor = vec4(texture(uSkybox, vDir).rgb, 1.0);
}
