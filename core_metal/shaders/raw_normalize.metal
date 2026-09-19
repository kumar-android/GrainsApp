#include <metal_stdlib>
using namespace metal;

kernel void raw_normalize(const device ushort *input [[buffer(0)]], device float *output [[buffer(1)]], constant float2 &levels [[buffer(2)]], uint id [[thread_position_in_grid]]) {
    output[id] = clamp((float(input[id]) - levels.x) / max(1.0f, levels.y - levels.x), 0.0f, 1.0f);
}
