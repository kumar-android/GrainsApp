#include <metal_stdlib>
using namespace metal;

kernel void demosaic(texture2d<float, access::read> raw [[texture(0)]], texture2d<float, access::write> rgb [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= rgb.get_width() || gid.y >= rgb.get_height()) return;
    float v = raw.read(gid).r;
    rgb.write(float4(v, v, v, 1.0f), gid);
}
