#version 450
void main() {
    vec2 first[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 other[3] = vec2[3](vec2(3.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, 3.0));
    gl_Position = vec4(gl_InstanceIndex == 0 ? other[gl_VertexIndex] : first[gl_VertexIndex], 0.0, 1.0);
}
