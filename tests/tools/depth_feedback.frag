#version 450
layout(set=0,binding=0) uniform sampler2D depth_image;
layout(location=0) out float value;
void main() {
    value = texelFetch(depth_image, ivec2(0,0), 0).r;
    gl_FragDepth = 0.75;
}
