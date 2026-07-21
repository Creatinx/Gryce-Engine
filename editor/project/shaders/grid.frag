#version 330 core

in vec3 vWorldPos;

out vec4 FragColor;

uniform vec3 uGridColor;
uniform float uGridSize;
uniform float uMajorLineEvery;
uniform float uFadeStart;
uniform float uFadeEnd;

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
