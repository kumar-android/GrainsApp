#include <metal_stdlib>
using namespace metal;

kernel void chroma_denoise(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> destination [[texture(1)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= destination.get_width() || gid.y >= destination.get_height()) return;
    destination.write(source.read(gid), gid);
}
