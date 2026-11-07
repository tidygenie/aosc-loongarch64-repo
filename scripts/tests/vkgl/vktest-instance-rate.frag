#version 450
layout(location=0) in vec2 uv;
layout(location=1) in float instance_variant;
layout(location=0) out vec4 color;
void main() {
    if (uv.x < -10.0 || uv.y < -10.0) {
        color = vec4(0.0, 0.0, 1.0, 1.0);
    } else if (instance_variant > 0.5) {
        color = vec4(1.0, 0.0, 0.0, 1.0);
    } else {
        color = vec4(0.0, 0.5, 0.0, 1.0);
    }
}
