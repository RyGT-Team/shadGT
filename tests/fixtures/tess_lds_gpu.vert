#version 450
layout(location = 0) out vec4 value[5];
layout(push_constant) uniform Push { float base; } pc;
void main() {
    for (int attr = 0; attr < 5; ++attr)
        value[attr] = pc.base + float(100 * attr + 10 * gl_VertexIndex) + vec4(0, 1, 2, 3);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
