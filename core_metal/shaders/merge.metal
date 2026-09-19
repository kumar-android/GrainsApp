#include <metal_stdlib>
using namespace metal;

kernel void merge(texture2d_array<float, access::read> frames [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= destination.get_width() || gid.y >= destination.get_height()) return;
    float4 sum = float4(0.0f);
    for (uint i = 0; i < frames.get_array_size(); ++i) sum += frames.read(gid, i);
    destination.write(sum / max(1.0f, float(frames.get_array_size())), gid);
}
