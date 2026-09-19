#include <metal_stdlib>
using namespace metal;

kernel void sharpen(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x == 0 || gid.y == 0 || gid.x + 1 >= source.get_width() || gid.y + 1 >= source.get_height()) return;
    float4 center = source.read(gid);
    float4 neighbours = source.read(gid + uint2(1, 0)) + source.read(gid - uint2(1, 0)) + source.read(gid + uint2(0, 1)) + source.read(gid - uint2(0, 1));
    destination.write(clamp(center + 0.08f * (center - neighbours * 0.25f), 0.0f, 1.0f), gid);
}
