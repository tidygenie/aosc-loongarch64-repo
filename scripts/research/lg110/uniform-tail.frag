#version 450
layout(set = 0, binding = 0) uniform Block {
    vec4 values[9];
} uniforms;
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = uniforms.values[8];
}
