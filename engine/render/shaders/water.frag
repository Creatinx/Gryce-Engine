#version 330 core
in vec2 vTexCoord;
in vec3 vWorldPos;
in vec3 vNormal;

uniform vec3 uCameraPos;
uniform vec3 uWaterColor;
uniform sampler2D uReflectionTex;
uniform sampler2D uRefractionTex;
uniform sampler2D uDepthTex;
uniform float uWaterHeight;
uniform float uFoamAmount;
uniform mat4 uViewProj;
uniform float uTime;
uniform float uWaveAmplitude;
uniform float uWaveFrequency;
uniform float uWaveSpeed;
uniform float uWaveSteepness;

out vec4 FragColor;

void main() {
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 N = normalize(vNormal);
    vec3 R = reflect(-V, N);
    
    // 反射/折射 UV（从世界空间投影到屏幕空间）
    vec4 clip_pos = uViewProj * vec4(vWorldPos, 1.0);
    vec2 uv = clip_pos.xy / clip_pos.w * 0.5 + 0.5;
    
    // 反射 UV 扰动（模拟水面波动对反射的影响）
    float disturb = sin(vWorldPos.x * 0.5 + vWorldPos.z * 0.3 + uTime * 0.8) * 0.01;
    vec2 reflect_uv = uv + vec2(disturb, disturb * 0.7);
    reflect_uv += R.xz * 0.05;
    
    // 采样反射
    vec3 reflection = texture(uReflectionTex, reflect_uv).rgb;
    
    // 采样折射
    vec3 refraction = texture(uRefractionTex, uv).rgb;
    
    // 菲涅尔（Schlick 近似）
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    fresnel = mix(0.1, 1.0, fresnel);
    
    // 泡沫（基于波浪高度）
    float foam = 0.0;
    float wave = sin(vWorldPos.x * uWaveFrequency * 0.8 + uTime * uWaveSpeed * 1.5) * 0.5 + 0.5;
    wave += sin(vWorldPos.z * uWaveFrequency * 0.6 + uTime * uWaveSpeed * 1.1) * 0.5 + 0.5;
    wave *= 0.5;
    foam = smoothstep(uFoamAmount * 0.8, uFoamAmount, wave);
    foam *= uFoamAmount;
    
    // 合成最终颜色
    vec3 color = mix(refraction, reflection, fresnel);
    color = mix(color, uWaterColor, 0.3);
    color += foam * vec3(1.0, 0.95, 0.9);
    
    // 半透明
    float alpha = mix(0.6, 0.9, fresnel);
    
    FragColor = vec4(color, alpha);
}