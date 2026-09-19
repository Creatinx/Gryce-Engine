#version 330 core

layout(location = 0) in vec3 aPos;

uniform mat4 uModelMatrix;
uniform mat4 uMVP;
uniform mat4 uPrevMVP;

out vec4 vCurrentPos;
out vec4 vPrevPos;

void main() {
    vCurrentPos = uMVP * vec4(aPos, 1.0);
    vPrevPos = uPrevMVP * vec4(aPos, 1.0);
    gl_Position = vCurrentPos;
}