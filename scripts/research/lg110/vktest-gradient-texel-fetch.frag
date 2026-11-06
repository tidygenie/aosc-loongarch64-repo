#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 gradient = vec3(0.0, gl_FragCoord.y / 480.0, 0.0);
    vec3 fetched = texelFetch(tex, ivec2(0, 0), 0).rgb;
    outColor = vec4(gradient + fetched * 0.000001, 1.0);
}
