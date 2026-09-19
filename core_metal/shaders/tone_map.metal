#include <metal_stdlib>
using namespace metal;

kernel void tone_map(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= destination.get_width() || gid.y >= destination.get_height()) return;
    float4 value = source.read(gid);
    value.rgb = value.rgb / (1.0f + value.rgb);
    destination.write(value, gid);
}
