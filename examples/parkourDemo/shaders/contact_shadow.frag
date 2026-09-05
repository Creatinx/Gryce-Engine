#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// 场景深度（主 pass 写入，Depth24）
uniform sampler2D uDepthTexture;
// 相机参数：由 render_contact_shadow() 用 set_uniform_float 传入
uniform float uCSNear;
uniform float uCSFar;
uniform float uCSTanHalfFov;
uniform float uCSAspect;
// 方向光方向（视图空间，指向光源），由 set_uniform_vec3 传入
uniform vec3 uCSLightDirView;
uniform float uCSRadius;   // 接触阴影世界半径（越小越只影响脚底接触处）
uniform int uCSteps;       // 步进数
uniform float uCSStrength; // 强度
uniform int uCSEnabled;    // 0 直接输出全亮

float linearize_depth(float d) {
    // 深度纹理存的是视图变换后的 [0,1] 深度，直接反算线性深度（勿再 *0.5+0.5）
    float d01 = d;
    return (2.0 * uCSNear * uCSFar) /
           (uCSFar + uCSNear - d01 * (uCSFar - uCSNear));
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * uCSTanHalfFov * uCSAspect * lin,
                ndc.y * uCSTanHalfFov * lin,
                -lin);
}

void main() {
    if (uCSEnabled == 0) { FragColor = vec4(1.0); return; }
    float lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    if (lin < 0.01 || lin >= uCSFar * 0.99) { FragColor = vec4(1.0); return; }

    vec3 P = reconstruct_view_pos(vTexCoord, lin);
    vec3 L = normalize(uCSLightDirView);
    float step_w = uCSRadius / float(max(uCSteps, 1));
    float occ = 0.0;
    for (int s = 1; s <= uCSteps; ++s) {
        vec3 Ps = P + L * (step_w * float(s));
        // 视图点投影回屏幕 uv
        vec2 proj = Ps.xy / (-Ps.z * uCSTanHalfFov);
        vec2 suv = proj * vec2(0.5 / uCSAspect, 0.5) + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        float lin2 = linearize_depth(texture(uDepthTexture, suv).r);
        if (lin2 < 0.01 || lin2 >= uCSFar * 0.99) continue;
        vec3 Q = reconstruct_view_pos(suv, lin2);

        // 视图空间 z 为负：Q 比 Ps 更靠近相机（Q.z > Ps.z）说明该方向有几何 → 接触遮挡
        if (Q.z > Ps.z + 1e-4) {
            occ += 1.0;
        }
    }
    float cs = 1.0 - (occ / float(max(uCSteps, 1))) * uCSStrength;
    FragColor = vec4(vec3(clamp(cs, 0.0, 1.0)), 1.0);
}