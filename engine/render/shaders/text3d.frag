#version 330 core

in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    // 字形图集是 RGBA8：RGB 恒为白、A 为字形覆盖度（见 FontAtlas 的打包方式）。
    // 因此把顶点色的 RGB 乘到字形颜色、顶点色的 A 乘到覆盖率即可。
    vec4 c = texture(uTexture, vUV);
    c.rgb *= vColor.rgb;
    c.a *= vColor.a;
    if (c.a <= 0.002) discard;
    FragColor = c;
}