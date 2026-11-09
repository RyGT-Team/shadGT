#version 450
layout(triangles, equal_spacing, cw) in;
layout(location = 0) in vec4 value[];
layout(set = 0, binding = 0, std430) buffer Result { uint words[]; } result;
void main() {
    for (int component = 0; component < 4; ++component)
        atomicExchange(result.words[4 * gl_PrimitiveID + component], floatBitsToUint(value[0][component]));
    gl_Position = vec4(gl_TessCoord.xy, 0.0, 1.0);
}
