#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform float uTime;
uniform float uWaveAmplitude;
uniform float uWaveFrequency;
uniform float uWaveSpeed;
uniform float uWaveSteepness;

out vec2 vTexCoord;
out vec3 vWorldPos;
out vec3 vNormal;

void main() {
    // 应用模型变换得到世界坐标
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vec3 pos = world_pos.xyz;

    // Gerstner 波: 多个正弦波叠加 + 水平位移（陡度）
    float freq1 = uWaveFrequency;
    float freq2 = uWaveFrequency * 1.3;
    float freq3 = uWaveFrequency * 0.5;

    float speed1 = uTime * uWaveSpeed;
    float speed2 = uTime * uWaveSpeed * 0.7;
    float speed3 = uTime * uWaveSpeed * 1.2;

    // 波相位
    float w1 = pos.x * freq1 + speed1;
    float w2 = pos.z * freq2 + speed2;
    float w3 = (pos.x + pos.z) * freq3 + speed3;

    // 垂直位移
    float wave1 = sin(w1) * uWaveAmplitude;
    float wave2 = sin(w2) * uWaveAmplitude * 0.6;
    float wave3 = sin(w3) * uWaveAmplitude * 0.3;
    pos.y += wave1 + wave2 + wave3;

    // 水平位移（Gerstner 陡度）
    float steep = uWaveSteepness * uWaveAmplitude;
    pos.x += cos(w1) * steep * 0.3;
    pos.z += cos(w2) * steep * 0.3;

    // 法线: 通过偏导数近似
    float dw1_dx = cos(w1) * freq1 * uWaveAmplitude;
    float dw2_dz = cos(w2) * freq2 * uWaveAmplitude * 0.6;
    float dw3_dx = cos(w3) * freq3 * uWaveAmplitude * 0.3;
    float dw3_dz = dw3_dx; // 因为 (x+z) 对 x 和 z 偏导相同
    vec3 normal = normalize(vec3(-(dw1_dx + dw3_dx), 1.0, -(dw2_dz + dw3_dz)));

    vTexCoord = aTexCoord * 10.0; // 平铺 UV
    vWorldPos = pos;
    vNormal = normal;

    gl_Position = uViewProj * vec4(pos, 1.0);
}