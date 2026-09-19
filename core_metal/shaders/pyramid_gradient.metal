#include <metal_stdlib>
using namespace metal;

kernel void pyramid_gradient(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x == 0 || gid.y == 0 || gid.x + 1 >= source.get_width() || gid.y + 1 >= source.get_height()) return;
    float center = source.read(gid).r;
    float gx = source.read(gid + uint2(1, 0)).r - source.read(gid - uint2(1, 0)).r;
    float gy = source.read(gid + uint2(0, 1)).r - source.read(gid - uint2(0, 1)).r;
    destination.write(float4(gx, gy, center, 1.0f), gid);
}
