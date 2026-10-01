#version 330 core

in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    // vColor.rgb = shaded 得到的明暗系数，vColor.a = 组件的不透明度。
    // 无贴图时绑定内置纯白回退贴图，统一按"贴图 × 顶点色"处理。
    vec4 c = texture(uTexture, vUV) * vColor;
    if (c.a <= 0.002) discard;
    FragColor = c;
}