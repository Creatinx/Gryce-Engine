#version 450 core

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 FragColor;

// Scene View 网格参数与 RenderPipeline 中的默认值保持一致。
const vec3 uGridColor = vec3(0.5, 0.5, 0.5);
const float uGridSize = 1.0;
const float uMajorLineEvery = 10.0;
const float uFadeStart = 30.0;
const float uFadeEnd = 100.0;

float grid_line(vec2 coord) {
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / derivative;
    return 1.0 - min(min(grid.x, grid.y), 1.0);
}

void main() {
    vec2 coord = vWorldPos.xz / uGridSize;

    float minor = grid_line(coord);
    float major = grid_line(coord / uMajorLineEvery);

    float alpha = max(minor * 0.25, major * 0.55);
    if (alpha <= 0.0) discard;

    float dist = length(vWorldPos.xz);
    alpha *= 1.0 - smoothstep(uFadeStart, uFadeEnd, dist);

    FragColor = vec4(uGridColor, alpha);
}
