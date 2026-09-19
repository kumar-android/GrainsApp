#import "MetalProcessor.h"
#import <Metal/Metal.h>

@interface GCamMetalProcessor ()
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLLibrary> library;
@property(nonatomic, strong) id<MTLComputePipelineState> rawLumaPipeline;
@end

@implementation GCamMetalProcessor

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _device = MTLCreateSystemDefaultDevice();
    _library = [_device newDefaultLibrary];
    id<MTLFunction> function = [_library newFunctionWithName:@"raw_luma_proxy"]; 
    if (function) _rawLumaPipeline = [_device newComputePipelineStateWithFunction:function error:nil];
    return self;
}

- (BOOL)isAvailable { return self.device != nil && self.rawLumaPipeline != nil; }

- (void)encodeRawLumaProxyForBuffer:(id<MTLCommandBuffer>)commandBuffer {
    if (!self.isAvailable || !commandBuffer) return;
    // Resource binding is supplied by the iOS capture adapter once the raw
    // buffer layout is known. CPU remains the correctness oracle.
}

@end
