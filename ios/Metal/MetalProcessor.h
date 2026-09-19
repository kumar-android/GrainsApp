#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

@interface GCamMetalProcessor : NSObject
- (instancetype)init NS_DESIGNATED_INITIALIZER;
- (BOOL)isAvailable;
- (void)encodeRawLumaProxyForBuffer:(id<MTLCommandBuffer>)commandBuffer;
@end

NS_ASSUME_NONNULL_END
