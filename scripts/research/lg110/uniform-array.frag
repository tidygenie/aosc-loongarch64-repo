#version 450
layout(set = 0, binding = 0) uniform Block {
    vec4 values[9];
} uniforms;
layout(location = 0) out vec4 FragColor;
void main() {
    float delta = (uniforms.values[0].x + uniforms.values[2].y +
                   uniforms.values[4].z + uniforms.values[5].w +
                   uniforms.values[6].x) / 16.0;
    FragColor = vec4(clamp(uniforms.values[8].rgb + vec3(delta), 0.0, 1.0),
                     uniforms.values[8].a);
}
