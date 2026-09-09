#version 330 core

in vec3 vWorldDir;

out vec4 FragColor;

uniform samplerCube uSkyboxTex;
uniform vec4 uSkyColor0;       // 天顶颜色
uniform vec4 uSkyColor1;       // 地平线颜色
uniform vec4 uSkyColor2;       // 底部颜色
uniform float uSunIntensity;   // 太阳强度
uniform vec3 uSunDirection;    // 太阳方向（归一化世界空间）
uniform int uUseTexturedSky;   // 0=程序化天空, 1=Cubemap

vec3 procedural_sky(vec3 dir) {
    float y = dir.y;

    // 水平渐变：天顶→地平线→底部
    float t;
    vec3 sky_color;
    if (y >= 0.0) {
        t = y;
        sky_color = mix(uSkyColor1.rgb, uSkyColor0.rgb, t);
    } else {
        t = -y;
        sky_color = mix(uSkyColor1.rgb, uSkyColor2.rgb, t);
    }

    // 地平线薄雾（增加大气感）
    float horizon = 1.0 - abs(y);
    horizon = pow(horizon, 2.0);
    sky_color = mix(sky_color, uSkyColor1.rgb * 1.5, horizon * 0.3);

    // 太阳光晕
    float sun_angle = max(dot(normalize(dir), normalize(uSunDirection)), 0.0);
    float sun_glow = pow(sun_angle, 128.0) * uSunIntensity;
    sky_color += vec3(1.0, 0.85, 0.6) * sun_glow;

    // 太阳周围光晕
    float sun_halo = pow(sun_angle, 4.0) * uSunIntensity * 0.15;
    sky_color += vec3(1.0, 0.9, 0.7) * sun_halo;

    return sky_color;
}

void main() {
    vec3 dir = normalize(vWorldDir);

    vec3 color;
    if (uUseTexturedSky != 0) {
        color = texture(uSkyboxTex, dir).rgb;
    } else {
        color = procedural_sky(dir);
    }

    FragColor = vec4(color, 1.0);
}