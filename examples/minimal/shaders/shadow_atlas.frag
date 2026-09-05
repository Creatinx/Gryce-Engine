#version 330 core

in vec2 vTexCoord;

uniform sampler2D uAlbedoMap;   // [Alpha Test] 材质反照率贴图，用于 alpha 裁剪
uniform int uUseAlbedoMap;      // [Alpha Test] 0=不使用纹理alpha，1=使用

void main() {
    // [Alpha Test] 对植被/栅栏等透明裁切材质，丢弃低 alpha 片元
    if (uUseAlbedoMap != 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a;
        if (alpha < 0.5) discard;
    }
    // depth 由 OpenGL 自动写入
}