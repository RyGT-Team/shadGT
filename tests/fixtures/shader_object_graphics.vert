#version 450
layout(set = 0, binding = 0) buffer Output { uint values[]; } outbuf;
layout(push_constant) uniform Push { uint base; uint offset; } push;
void main() {
    outbuf.values[push.offset + gl_VertexIndex] = push.base + uint(gl_VertexIndex);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
