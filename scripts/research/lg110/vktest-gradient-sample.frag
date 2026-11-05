#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 gradient = vec3(0.0, gl_FragCoord.y / 480.0, 0.0);
    vec3 sampled = texture(tex, vec2(0.25, 0.25)).rgb;
    outColor = vec4(gradient + sampled * 0.000001, 1.0);
}
