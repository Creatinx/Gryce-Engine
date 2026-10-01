#version 330 core

in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    // 无贴图时绑定 1x1 白色回退贴图，这里统一按"贴图 × 顶点色"处理，
    // 免掉 uUseTexture 分支（Vulkan 侧的 per-object UBO 没有 int 字段位）。
    vec4 c = vColor * texture(uTexture, vUV);
    if (c.a <= 0.002) discard;
    FragColor = c;
}