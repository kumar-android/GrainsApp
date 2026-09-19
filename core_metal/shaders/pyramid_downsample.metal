#include <metal_stdlib>
using namespace metal;

kernel void pyramid_downsample(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= destination.get_width() || gid.y >= destination.get_height()) return;
    uint2 origin = gid * 2;
    float4 value = source.read(origin) + source.read(origin + uint2(1, 0)) + source.read(origin + uint2(0, 1)) + source.read(origin + uint2(1, 1));
    destination.write(value * 0.25f, gid);
}
