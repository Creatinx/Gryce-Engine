#version 330 core

layout(location = 0) in vec3 aPos;

out vec3 vWorldPos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz;
    gl_Position = uProjection * uView * world_pos;
}
