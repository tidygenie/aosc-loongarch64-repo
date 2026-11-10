#version 450
void main() {
    vec2 positions[6] = vec2[6](
        vec2(-2.0, -3.0), vec2(-2.0, -2.0), vec2(2.0, -2.0),
        vec2(0.0, 2.0), vec2(3.0, 3.0), vec2(4.0, 2.0));
    // Both 0xffff and 0xffffffff map to vertex 3. This keeps the disabled-restart
    // control in bounds while making its connecting triangle 1/2/3 cover center.
    gl_Position = vec4(positions[gl_VertexIndex > 5 ? 3 : gl_VertexIndex], 0.0, 1.0);
}
