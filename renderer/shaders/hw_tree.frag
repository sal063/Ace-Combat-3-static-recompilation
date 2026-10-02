#version 450
#extension GL_GOOGLE_include_directive : require

#include "hw_fx_common.glsl"

layout(location = 0) in vec3 v_col;
layout(location = 1) in vec3 v_n;
layout(location = 2) in float v_ao;

layout(location = 0) out vec4 o_col;

void main() {
    vec3 n = normalize(v_n);
    float l = max(dot(n, U.sun.xyz), 0.0);
    float up = clamp(-n.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = sky_at(1.0);
    vec3 light = vec3(0.62) + U.sun_col.rgb * (0.55 * l) + sky * (0.12 * up);
    o_col = vec4(v_col * light * v_ao, 0.0);
}
