#version 450
layout(set = 0, binding = 0) uniform ColorBlock {
    vec4 value;
} color;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = color.value;
}
