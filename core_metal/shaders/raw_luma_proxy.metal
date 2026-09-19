#include <metal_stdlib>
using namespace metal;

kernel void raw_luma_proxy(const device float *raw [[buffer(0)]], device float *luma [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    luma[id] = raw[id];
}
