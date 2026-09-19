#version 450 core

layout(location = 0) in vec2 vTexCoord;

// 材质 UBO：alpha test 所需字段
layout(set = 0, binding = 0, std140) uniform MaterialUBO {
    layout(offset = 96) int uUseAlbedoMap;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D uAlbedoMap;

void main() {
    // [Alpha Test] 对植被/栅栏等透明裁切材质，丢弃低 alpha 片元
    if (ubo.uUseAlbedoMap != 0) {
        float alpha = texture(uAlbedoMap, vTexCoord).a;
        if (alpha < 0.5) discard;
    }
    // depth 由深度附件自动写入
}