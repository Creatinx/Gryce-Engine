#version 330 core

layout(location = 0) in vec3 aPos;

uniform mat4 uModelMatrix;
uniform mat4 uViewMatrix;
uniform mat4 uProjectionMatrix;

void main() {
    vec4 world_pos = uModelMatrix * vec4(aPos, 1.0);
    gl_Position = uProjectionMatrix * uViewMatrix * world_pos;
}