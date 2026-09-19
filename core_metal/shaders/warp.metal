#include <metal_stdlib>
using namespace metal;

kernel void warp(texture2d<float, access::sample> source [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], constant float2 &offset [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= destination.get_width() || gid.y >= destination.get_height()) return;
    constexpr sampler linearSampler(coord::pixel, address::clamp_to_edge, filter::linear);
    destination.write(source.sample(linearSampler, float2(gid) + offset), gid);
}
