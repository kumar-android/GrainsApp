#include <metal_stdlib>
using namespace metal;

kernel void robust_merge(texture2d_array<float, access::read> frames [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= destination.get_width() || gid.y >= destination.get_height()) return;
    float4 reference = frames.read(gid, 0);
    float4 sum = reference;
    float weightSum = 1.0f;
    for (uint i = 1; i < frames.get_array_size(); ++i) {
        float4 sample = frames.read(gid, i);
        float residual = length(sample - reference);
        float weight = residual < 0.05f ? 1.0f : 0.05f / max(residual, 0.05f);
        sum += sample * weight;
        weightSum += weight;
    }
    destination.write(sum / weightSum, gid);
}
