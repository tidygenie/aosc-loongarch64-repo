#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 out_color;
void main() {
    vec3 gradient = vec3(0.0, gl_FragCoord.y / 480.0, 0.0);
    vec3 sampled = texture(tex, vec2(0.2505)).xyz;
    out_color = vec4(gradient + sampled * 0.25, 1.0);
}
