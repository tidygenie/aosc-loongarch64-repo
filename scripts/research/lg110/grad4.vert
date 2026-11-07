#version 450
// draw3/first0 covers the center; draw3/first1 selects vertices 1,2,3,
// whose complementary triangle does not cover it. All indices stay in bounds.
void main() {
    vec2 pos[4] = vec2[4](vec2(-1.0, -1.0), vec2(3.0, -1.0),
                          vec2(-1.0, 3.0), vec2(3.0, 3.0));
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
