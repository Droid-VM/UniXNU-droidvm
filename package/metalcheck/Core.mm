#import "Core.h"

#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <simd/simd.h>
#import <mach/mach.h>
#include <sys/mman.h>
#include <vector>
#import <objc/message.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

@interface MCContext : NSObject
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLLibrary> library;
@end

@implementation MCContext
@end

static NSString * const MCFailureName = @"MetalCheckFailure";
static NSString * const MCSkipName = @"MetalCheckSkip";
static NSString * const MCAbortName = @"MetalCheckAbort";
static const NSUInteger MCCommandTimeoutSeconds = 10;

static void MCRequire(BOOL condition, NSString *reason) {
    if (!condition) {
        @throw [NSException exceptionWithName:MCFailureName reason:reason userInfo:nil];
    }
}

static void MCSkip(NSString *reason) {
    @throw [NSException exceptionWithName:MCSkipName reason:reason userInfo:nil];
}

static void MCAbort(NSString *reason) {
    @throw [NSException exceptionWithName:MCAbortName reason:reason userInfo:nil];
}

static id<MTLBuffer> MCBuffer(MCContext *context, NSUInteger length, MTLResourceOptions options) {
    id<MTLBuffer> buffer = [context.device newBufferWithLength:length options:options];
    MCRequire(buffer != nil, [NSString stringWithFormat:@"newBufferWithLength:%lu returned nil", (unsigned long)length]);
    return buffer;
}

static id<MTLTexture> MCTexture(MCContext *context, MTLPixelFormat format, NSUInteger width,
                                NSUInteger height, MTLStorageMode storageMode,
                                MTLTextureUsage usage, BOOL mipmapped) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                           width:width
                                                                                          height:height
                                                                                       mipmapped:mipmapped];
    descriptor.storageMode = storageMode;
    descriptor.usage = usage;
    id<MTLTexture> texture = [context.device newTextureWithDescriptor:descriptor];
    MCRequire(texture != nil, [NSString stringWithFormat:@"newTextureWithDescriptor: returned nil for %lux%lu %@",
                               (unsigned long)width, (unsigned long)height,
                               MTLPixelFormatRGBA8Unorm == format ? @"RGBA8Unorm" : @"requested format"]);
    return texture;
}

static id<MTLFunction> MCFunction(MCContext *context, NSString *name) {
    id<MTLFunction> function = [context.library newFunctionWithName:name];
    MCRequire(function != nil, [NSString stringWithFormat:@"newFunctionWithName:%@ returned nil", name]);
    return function;
}

static id<MTLComputePipelineState> MCComputePipeline(MCContext *context, NSString *name) {
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = [context.device newComputePipelineStateWithFunction:MCFunction(context, name)
                                                                                         error:&error];
    MCRequire(pipeline != nil, [NSString stringWithFormat:@"compute pipeline %@ failed: %@", name,
                                error.localizedDescription != nil ? error.localizedDescription : @"no NSError"]);
    return pipeline;
}

static id<MTLRenderPipelineState> MCRenderPipeline(MCContext *context, MTLRenderPipelineDescriptor *descriptor,
                                                   NSString *label) {
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [context.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    MCRequire(pipeline != nil, [NSString stringWithFormat:@"render pipeline %@ failed: %@", label,
                                error.localizedDescription != nil ? error.localizedDescription : @"no NSError"]);
    return pipeline;
}

static void MCWaitForCommandBuffer(id<MTLCommandBuffer> commandBuffer) {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        (void)completed;
        dispatch_semaphore_signal(semaphore);
    }];
    [commandBuffer commit];
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW,
                                             (int64_t)MCCommandTimeoutSeconds * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(semaphore, deadline) != 0) {
        MCAbort([NSString stringWithFormat:@"command buffer timed out after %lu seconds",
                 (unsigned long)MCCommandTimeoutSeconds]);
    }
    MCRequire(commandBuffer.status == MTLCommandBufferStatusCompleted && commandBuffer.error == nil,
              [NSString stringWithFormat:@"command buffer status %lu: %@",
               (unsigned long)commandBuffer.status,
               commandBuffer.error.localizedDescription != nil ? commandBuffer.error.localizedDescription : @"no NSError"]);
}

static void MCWaitForCommandBuffers(NSArray<id<MTLCommandBuffer>> *commandBuffers, BOOL commit) {
    NSMutableArray<dispatch_semaphore_t> *semaphores = [NSMutableArray arrayWithCapacity:commandBuffers.count];
    for (id<MTLCommandBuffer> commandBuffer in commandBuffers) {
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        [semaphores addObject:semaphore];
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            (void)completed;
            dispatch_semaphore_signal(semaphore);
        }];
    }
    if (commit) {
        for (id<MTLCommandBuffer> commandBuffer in commandBuffers) [commandBuffer commit];
    }
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW,
                                             (int64_t)MCCommandTimeoutSeconds * NSEC_PER_SEC);
    for (NSUInteger index = 0; index < commandBuffers.count; index++) {
        if (dispatch_semaphore_wait(semaphores[index], deadline) != 0) {
            MCAbort([NSString stringWithFormat:@"command buffer %lu timed out after %lu seconds",
                     (unsigned long)index, (unsigned long)MCCommandTimeoutSeconds]);
        }
    }
    // Drain every submitted buffer before reporting an execution error. A failed
    // producer can strand an event consumer, which must take the abort/timeout path.
    for (NSUInteger index = 0; index < commandBuffers.count; index++) {
        id<MTLCommandBuffer> commandBuffer = commandBuffers[index];
        MCRequire(commandBuffer.status == MTLCommandBufferStatusCompleted && commandBuffer.error == nil,
                  [NSString stringWithFormat:@"command buffer %lu status %lu: %@",
                   (unsigned long)index, (unsigned long)commandBuffer.status,
                   commandBuffer.error.localizedDescription != nil ? commandBuffer.error.localizedDescription : @"no NSError"]);
    }
}

static NSUInteger MCAlign(NSUInteger value, NSUInteger alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static NSData *MCReadPrivateTexture(MCContext *context, id<MTLTexture> texture,
                                    NSUInteger slice, NSUInteger level) {
    NSUInteger width = MAX((NSUInteger)1, texture.width >> level);
    NSUInteger height = MAX((NSUInteger)1, texture.height >> level);
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    NSUInteger imageBytes = rowBytes * height;
    id<MTLBuffer> readback = MCBuffer(context, imageBytes, MTLResourceStorageModeShared);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandQueue commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    [blit copyFromTexture:texture
              sourceSlice:slice
              sourceLevel:level
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:readback
        destinationOffset:0
   destinationBytesPerRow:rowBytes
 destinationBytesPerImage:imageBytes];
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    return [NSData dataWithBytes:readback.contents length:imageBytes];
}

static NSData *MCReadSharedTexture(id<MTLTexture> texture, NSUInteger slice, NSUInteger level,
                                  NSUInteger rowBytes) {
    NSUInteger width = MAX((NSUInteger)1, texture.width >> level);
    NSUInteger height = MAX((NSUInteger)1, texture.height >> level);
    NSMutableData *data = [NSMutableData dataWithLength:rowBytes * height];
    [texture getBytes:data.mutableBytes
          bytesPerRow:rowBytes
        bytesPerImage:rowBytes * height
           fromRegion:MTLRegionMake2D(0, 0, width, height)
          mipmapLevel:level
                slice:slice];
    return data;
}

static const uint8_t *MCPixel(NSData *data, NSUInteger rowBytes, NSUInteger x, NSUInteger y) {
    return static_cast<const uint8_t *>(data.bytes) + y * rowBytes + x * 4;
}

static void MCRequirePixel(NSData *data, NSUInteger rowBytes, NSUInteger x, NSUInteger y,
                           const uint8_t expected[4], uint8_t tolerance, NSString *label) {
    const uint8_t *actual = MCPixel(data, rowBytes, x, y);
    for (NSUInteger channel = 0; channel < 4; channel++) {
        int delta = (int)actual[channel] - (int)expected[channel];
        MCRequire(std::abs(delta) <= tolerance,
                  [NSString stringWithFormat:@"%@ pixel (%lu,%lu) channel %lu expected %u±%u got %u",
                   label, (unsigned long)x, (unsigned long)y, (unsigned long)channel,
                   (unsigned)expected[channel], (unsigned)tolerance, (unsigned)actual[channel]]);
    }
}

static MTLRenderPassDescriptor *MCRenderPass(id<MTLTexture> colorTexture, MTLLoadAction loadAction,
                                             MTLStoreAction storeAction) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = colorTexture;
    pass.colorAttachments[0].loadAction = loadAction;
    pass.colorAttachments[0].storeAction = storeAction;
    return pass;
}

static id<MTLRenderPipelineState> MCSimpleRenderPipeline(MCContext *context, NSString *vertexName,
                                                        NSString *fragmentName, MTLPixelFormat colorFormat,
                                                        NSUInteger sampleCount) {
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, vertexName);
    descriptor.fragmentFunction = MCFunction(context, fragmentName);
    descriptor.colorAttachments[0].pixelFormat = colorFormat;
    descriptor.rasterSampleCount = sampleCount;
    return MCRenderPipeline(context, descriptor, [NSString stringWithFormat:@"%@/%@", vertexName, fragmentName]);
}

static MTLVertexDescriptor *MCQuadVertexDescriptor(void) {
    MTLVertexDescriptor *descriptor = [[MTLVertexDescriptor alloc] init];
    descriptor.attributes[0].format = MTLVertexFormatFloat2;
    descriptor.attributes[0].offset = 0;
    descriptor.attributes[0].bufferIndex = 0;
    descriptor.layouts[0].stride = sizeof(simd_float2);
    descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    descriptor.layouts[0].stepRate = 1;
    return descriptor;
}

static void MCCheckComputeResult(id<MTLBuffer> input, id<MTLBuffer> output, NSUInteger count,
                                BOOL affine, NSString *label) {
    const float *inValues = static_cast<const float *>(input.contents);
    const float *outValues = static_cast<const float *>(output.contents);
    for (NSUInteger index = 0; index < count; index++) {
        float expected = affine ? 3.0f * inValues[index] + 7.0f : inValues[index];
        MCRequire(std::fabs(outValues[index] - expected) < 0.0001f,
                  [NSString stringWithFormat:@"%@ index %lu expected %.6f got %.6f",
                   label, (unsigned long)index, expected, outValues[index]]);
    }
}

static void MCTest01(MCContext *context) {
    id<MTLBuffer> buffer = MCBuffer(context, 64, MTLResourceStorageModeShared);
    uint32_t *values = static_cast<uint32_t *>(buffer.contents);
    for (uint32_t index = 0; index < 16; index++) values[index] = index * 13u + 5u;
    values[7] = 0xdecafbad;
    MCRequire(values[7] == 0xdecafbad && values[15] == 200u, @"shared buffer CPU contents did not round-trip");
}

static void MCTest02(MCContext *context) {
    id<MTLBuffer> buffer = MCBuffer(context, 128, MTLResourceStorageModeShared);
    std::memset(buffer.contents, 0xa7, buffer.length);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    [blit fillBuffer:buffer range:NSMakeRange(24, 56) value:0x5c];
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer.contents);
    for (NSUInteger index = 0; index < buffer.length; index++) {
        uint8_t expected = (index >= 24 && index < 80) ? 0x5c : 0xa7;
        MCRequire(bytes[index] == expected,
                  [NSString stringWithFormat:@"partial fill guard/value mismatch at byte %lu", (unsigned long)index]);
    }
}

static void MCTest03(MCContext *context) {
    const NSUInteger copySize = 72;
    const NSUInteger sourceOffset = 12;
    const NSUInteger privateOffset = 8;
    const NSUInteger destinationOffset = 28;
    id<MTLBuffer> source = MCBuffer(context, 128, MTLResourceStorageModeShared);
    id<MTLBuffer> middle = MCBuffer(context, 96, MTLResourceStorageModePrivate);
    id<MTLBuffer> destination = MCBuffer(context, 160, MTLResourceStorageModeShared);
    uint8_t *sourceBytes = static_cast<uint8_t *>(source.contents);
    uint8_t *destinationBytes = static_cast<uint8_t *>(destination.contents);
    for (NSUInteger index = 0; index < source.length; index++) sourceBytes[index] = (uint8_t)(index * 19u + 7u);
    std::memset(destinationBytes, 0xd3, destination.length);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    [blit copyFromBuffer:source sourceOffset:sourceOffset toBuffer:middle destinationOffset:privateOffset size:copySize];
    [blit copyFromBuffer:middle sourceOffset:privateOffset toBuffer:destination destinationOffset:destinationOffset size:copySize];
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    for (NSUInteger index = 0; index < destination.length; index++) {
        uint8_t expected = index >= destinationOffset && index < destinationOffset + copySize
            ? sourceBytes[sourceOffset + index - destinationOffset] : 0xd3;
        MCRequire(destinationBytes[index] == expected,
                  [NSString stringWithFormat:@"offset copy/guard mismatch at byte %lu", (unsigned long)index]);
    }
}

static void MCRunAffineCase(MCContext *context, BOOL useThreadgroups) {
    const uint32_t count = 1003;
    id<MTLBuffer> input = MCBuffer(context, count * sizeof(float), MTLResourceStorageModeShared);
    id<MTLBuffer> output = MCBuffer(context, count * sizeof(float), MTLResourceStorageModeShared);
    float *inputValues = static_cast<float *>(input.contents);
    float *outputValues = static_cast<float *>(output.contents);
    for (uint32_t index = 0; index < count; index++) {
        inputValues[index] = (float)((int)index - 501) * 0.125f;
        outputValues[index] = -9999.0f;
    }
    id<MTLComputePipelineState> pipeline = MCComputePipeline(context, @"mc_affine");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    MCRequire(encoder != nil, @"computeCommandEncoder returned nil");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    if (useThreadgroups) {
        NSUInteger groups = (count + 63u) / 64u;
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    } else {
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    }
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    MCCheckComputeResult(input, output, count, YES, useThreadgroups ? @"threadgroups affine" : @"dispatchThreads affine");
}

static void MCTest04(MCContext *context) { MCRunAffineCase(context, NO); }
static void MCTest05(MCContext *context) { MCRunAffineCase(context, YES); }

static void MCTest06(MCContext *context) {
    const uint32_t count = 1024;
    uint32_t groups[3] = {16, 1, 1};
    id<MTLBuffer> input = MCBuffer(context, count * sizeof(float), MTLResourceStorageModeShared);
    id<MTLBuffer> output = MCBuffer(context, count * sizeof(float), MTLResourceStorageModeShared);
    // Populate private arguments on the GPU before dispatch, at a nonzero offset.
    // Reading these arguments on the CPU while encoding would use stale data.
    const NSUInteger argumentOffset = 16;
    id<MTLBuffer> arguments = MCBuffer(context, sizeof(groups), MTLResourceStorageModeShared);
    id<MTLBuffer> indirect = MCBuffer(context, argumentOffset + sizeof(groups), MTLResourceStorageModePrivate);
    float *inputValues = static_cast<float *>(input.contents);
    float *outputValues = static_cast<float *>(output.contents);
    for (uint32_t index = 0; index < count; index++) {
        inputValues[index] = (float)index * 0.25f - 3.0f;
        outputValues[index] = -1.0f;
    }
    std::memcpy(arguments.contents, groups, sizeof(groups));
    id<MTLComputePipelineState> pipeline = MCComputePipeline(context, @"mc_affine");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"indirect argument blit encoder returned nil");
    [blit copyFromBuffer:arguments sourceOffset:0 toBuffer:indirect destinationOffset:argumentOffset size:sizeof(groups)];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    MCRequire(encoder != nil, @"computeCommandEncoder returned nil");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreadgroupsWithIndirectBuffer:indirect indirectBufferOffset:argumentOffset
                              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    MCCheckComputeResult(input, output, count, YES, @"indirect affine");
}

static void MCTest07(MCContext *context) {
    const NSUInteger width = 7, height = 5, rowBytes = 256;
    id<MTLTexture> texture = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                       MTLStorageModeShared, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, NO);
    NSMutableData *source = [NSMutableData dataWithLength:rowBytes * height];
    uint8_t *sourceBytes = static_cast<uint8_t *>(source.mutableBytes);
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width; x++) {
            uint8_t *pixel = sourceBytes + y * rowBytes + x * 4;
            pixel[0] = (uint8_t)(x * 23u + 3u);
            pixel[1] = (uint8_t)(y * 31u + 5u);
            pixel[2] = (uint8_t)(x + y * 7u);
            pixel[3] = 255;
        }
    }
    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [texture replaceRegion:region mipmapLevel:0 withBytes:source.bytes bytesPerRow:rowBytes];
    NSData *readback = MCReadSharedTexture(texture, 0, 0, rowBytes);
    const uint8_t *readBytes = static_cast<const uint8_t *>(readback.bytes);
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width * 4; x++) {
            MCRequire(readBytes[y * rowBytes + x] == sourceBytes[y * rowBytes + x],
                      [NSString stringWithFormat:@"replaceRegion/getBytes mismatch at row %lu byte %lu",
                       (unsigned long)y, (unsigned long)x]);
        }
    }
}

static void MCTest08(MCContext *context) {
    const NSUInteger width = 7, height = 4, rowBytes = 256;
    const NSUInteger sourceOffset = 36, destinationOffset = 20, guard = 32;
    const uint8_t guardValue = 0xe7;
    id<MTLBuffer> source = MCBuffer(context, sourceOffset + rowBytes * height + guard, MTLResourceStorageModeShared);
    id<MTLBuffer> destination = MCBuffer(context, destinationOffset + rowBytes * height + guard,
                                         MTLResourceStorageModeShared);
    id<MTLTexture> texture = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                       MTLStorageModePrivate, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, NO);
    uint8_t *sourceBytes = static_cast<uint8_t *>(source.contents);
    uint8_t *destinationBytes = static_cast<uint8_t *>(destination.contents);
    std::memset(sourceBytes, 0x91, source.length);
    std::memset(destinationBytes, guardValue, destination.length);
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width; x++) {
            uint8_t *pixel = sourceBytes + sourceOffset + y * rowBytes + x * 4;
            pixel[0] = (uint8_t)(x * 17u + 1u);
            pixel[1] = (uint8_t)(y * 29u + 2u);
            pixel[2] = (uint8_t)(x + y * 3u);
            pixel[3] = 255;
        }
    }
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    MTLSize size = MTLSizeMake(width, height, 1);
    [blit copyFromBuffer:source sourceOffset:sourceOffset sourceBytesPerRow:rowBytes
      sourceBytesPerImage:rowBytes * height sourceSize:size toTexture:texture destinationSlice:0
         destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:size toBuffer:destination destinationOffset:destinationOffset
   destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * height];
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    for (NSUInteger index = 0; index < destinationOffset; index++)
        MCRequire(destinationBytes[index] == guardValue, @"destination prefix guard changed");
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width * 4; x++) {
            uint8_t expected = sourceBytes[sourceOffset + y * rowBytes + x];
            MCRequire(destinationBytes[destinationOffset + y * rowBytes + x] == expected,
                      [NSString stringWithFormat:@"buffer-texture-buffer pixel bytes mismatch row %lu byte %lu",
                       (unsigned long)y, (unsigned long)x]);
        }
    }
    for (NSUInteger index = destinationOffset + rowBytes * height; index < destination.length; index++)
        MCRequire(destinationBytes[index] == guardValue, @"destination suffix guard changed");
}

static void MCTest09(MCContext *context) {
    const NSUInteger width = 8, height = 6;
    id<MTLTexture> source = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                      MTLStorageModeShared, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, NO);
    id<MTLTexture> destination = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                           MTLStorageModeShared, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, NO);
    NSMutableData *sourceData = [NSMutableData dataWithLength:width * height * 4];
    NSMutableData *destinationData = [NSMutableData dataWithLength:width * height * 4];
    uint8_t *sourceBytes = static_cast<uint8_t *>(sourceData.mutableBytes);
    uint8_t *destinationBytes = static_cast<uint8_t *>(destinationData.mutableBytes);
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width; x++) {
            NSUInteger offset = (y * width + x) * 4;
            sourceBytes[offset + 0] = (uint8_t)(x * 21u + 3u);
            sourceBytes[offset + 1] = (uint8_t)(y * 27u + 4u);
            sourceBytes[offset + 2] = (uint8_t)(x + y * 5u);
            sourceBytes[offset + 3] = 255;
            destinationBytes[offset + 0] = 9;
            destinationBytes[offset + 1] = 11;
            destinationBytes[offset + 2] = 13;
            destinationBytes[offset + 3] = 255;
        }
    }
    MTLRegion fullRegion = MTLRegionMake2D(0, 0, width, height);
    [source replaceRegion:fullRegion mipmapLevel:0 withBytes:sourceBytes bytesPerRow:width * 4];
    [destination replaceRegion:fullRegion mipmapLevel:0 withBytes:destinationBytes bytesPerRow:width * 4];
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(1, 1, 0)
               sourceSize:MTLSizeMake(3, 2, 1) toTexture:destination destinationSlice:0 destinationLevel:0
       destinationOrigin:MTLOriginMake(4, 3, 0)];
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadSharedTexture(destination, 0, 0, width * 4);
    const uint8_t *actual = static_cast<const uint8_t *>(readback.bytes);
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width; x++) {
            BOOL copied = x >= 4 && x < 7 && y >= 3 && y < 5;
            NSUInteger sourceX = copied ? x - 3 : x;
            NSUInteger sourceY = copied ? y - 2 : y;
            NSUInteger sourceOffset = (sourceY * width + sourceX) * 4;
            NSUInteger destinationOffset = (y * width + x) * 4;
            const uint8_t *expected = copied ? sourceBytes + sourceOffset : destinationBytes + destinationOffset;
            for (NSUInteger channel = 0; channel < 4; channel++) {
                MCRequire(actual[destinationOffset + channel] == expected[channel],
                          [NSString stringWithFormat:@"texture region copy mismatch at (%lu,%lu) channel %lu",
                           (unsigned long)x, (unsigned long)y, (unsigned long)channel]);
            }
        }
    }
}

static void MCTest10(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t expected[4] = {64, 128, 191, 255};
    MCRequirePixel(readback, MCAlign(width * 4, 256), 16, 16, expected, 1, @"private clear");
}

static void MCTest11(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    id<MTLRenderPipelineState> pipeline = MCSimpleRenderPipeline(context, @"mc_fullscreen_vertex",
                                                                 @"mc_red_fragment", MTLPixelFormatRGBA8Unorm, 1);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t red[4] = {255, 0, 0, 255};
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    MCRequirePixel(readback, rowBytes, 16, 16, red, 0, @"fullscreen triangle");
    MCRequirePixel(readback, rowBytes, 0, 0, red, 0, @"fullscreen triangle corner");
}

static void MCTest12(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    simd_float2 positions[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    id<MTLBuffer> vertices = MCBuffer(context, sizeof(positions), MTLResourceStorageModeShared);
    id<MTLBuffer> indexBuffer = MCBuffer(context, sizeof(indices), MTLResourceStorageModeShared);
    std::memcpy(vertices.contents, positions, sizeof(positions));
    std::memcpy(indexBuffer.contents, indices, sizeof(indices));
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_quad_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_red_fragment");
    descriptor.vertexDescriptor = MCQuadVertexDescriptor();
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"vertex descriptor indexed quad");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vertices offset:0 atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16
                        indexBuffer:indexBuffer indexBufferOffset:0];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t red[4] = {255, 0, 0, 255};
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width; x++) {
            MCRequirePixel(readback, rowBytes, x, y, red, 0, @"indexed quad per-pixel red");
        }
    }
}

static void MCTest13(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    simd_float2 positions[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    id<MTLBuffer> vertices = MCBuffer(context, sizeof(positions), MTLResourceStorageModeShared);
    id<MTLBuffer> indexBuffer = MCBuffer(context, sizeof(indices), MTLResourceStorageModeShared);
    std::memcpy(vertices.contents, positions, sizeof(positions));
    std::memcpy(indexBuffer.contents, indices, sizeof(indices));
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_instanced_quad_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_vertex_color_fragment");
    descriptor.vertexDescriptor = MCQuadVertexDescriptor();
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"two instance halves");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:vertices offset:0 atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16
                        indexBuffer:indexBuffer indexBufferOffset:0 instanceCount:2];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    MCRequirePixel(readback, rowBytes, 7, 16, red, 0, @"instance 0 left half");
    MCRequirePixel(readback, rowBytes, 24, 16, green, 0, @"instance 1 right half");
}

static void MCTest14(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_alpha_fragment");
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"alpha blend red over blue");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t expected[4] = {128, 0, 127, 255};
    MCRequirePixel(readback, MCAlign(width * 4, 256), 16, 16, expected, 1, @"alpha blend");
}

static void MCTest15(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    id<MTLRenderPipelineState> pipeline = MCSimpleRenderPipeline(context, @"mc_fullscreen_vertex",
                                                                 @"mc_red_fragment", MTLPixelFormatRGBA8Unorm, 1);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    MTLScissorRect scissor = {12, 12, 8, 8};
    [encoder setScissorRect:scissor];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t blue[4] = {0, 0, 255, 255};
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    MCRequirePixel(readback, rowBytes, 16, 16, red, 0, @"scissor interior");
    MCRequirePixel(readback, rowBytes, 4, 4, blue, 0, @"scissor exterior");
    MCRequirePixel(readback, rowBytes, 28, 28, blue, 0, @"scissor opposite exterior");
}

static void MCTest16(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    MTLTextureDescriptor *depthDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                                 width:width height:height mipmapped:NO];
    depthDescriptor.storageMode = MTLStorageModePrivate;
    depthDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> depth = [context.device newTextureWithDescriptor:depthDescriptor];
    MCRequire(depth != nil, @"Depth32Float render target creation returned nil");
    MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.vertexFunction = MCFunction(context, @"mc_depth_vertex");
    pipelineDescriptor.fragmentFunction = MCFunction(context, @"mc_vertex_color_fragment");
    pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, pipelineDescriptor, @"depth less");
    MTLDepthStencilDescriptor *depthDescriptorState = [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptorState.depthCompareFunction = MTLCompareFunctionLess;
    depthDescriptorState.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depthState = [context.device newDepthStencilStateWithDescriptor:depthDescriptorState];
    MCRequire(depthState != nil, @"newDepthStencilStateWithDescriptor: returned nil for depth less");

    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 1.0;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:depthState];
    const float depths[3] = {0.8f, 0.2f, 0.8f};
    const float colors[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (NSUInteger draw = 0; draw < 3; draw++) {
        [encoder setVertexBytes:&depths[draw] length:sizeof(float) atIndex:0];
        [encoder setVertexBytes:colors[draw] length:sizeof(colors[draw]) atIndex:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t green[4] = {0, 255, 0, 255};
    MCRequirePixel(readback, MCAlign(width * 4, 256), 16, 16, green, 0, @"depth less keeps near green");
}

static MTLStencilDescriptor *MCStencilDescriptor(MTLCompareFunction compare, MTLStencilOperation passOperation) {
    MTLStencilDescriptor *descriptor = [[MTLStencilDescriptor alloc] init];
    descriptor.stencilCompareFunction = compare;
    descriptor.stencilFailureOperation = MTLStencilOperationKeep;
    descriptor.depthFailureOperation = MTLStencilOperationKeep;
    descriptor.depthStencilPassOperation = passOperation;
    descriptor.readMask = 0xff;
    descriptor.writeMask = 0xff;
    return descriptor;
}

static id<MTLDepthStencilState> MCStencilState(MCContext *context, MTLCompareFunction compare,
                                              MTLStencilOperation passOperation) {
    MTLDepthStencilDescriptor *descriptor = [[MTLDepthStencilDescriptor alloc] init];
    descriptor.depthCompareFunction = MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = NO;
    descriptor.frontFaceStencil = MCStencilDescriptor(compare, passOperation);
    descriptor.backFaceStencil = MCStencilDescriptor(compare, passOperation);
    id<MTLDepthStencilState> state = [context.device newDepthStencilStateWithDescriptor:descriptor];
    MCRequire(state != nil, @"newDepthStencilStateWithDescriptor: returned nil for stencil");
    return state;
}

static void MCTest17(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> color = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    MTLTextureDescriptor *stencilDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8
                                                                                                  width:width height:height mipmapped:NO];
    stencilDescriptor.storageMode = MTLStorageModePrivate;
    stencilDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> stencil = [context.device newTextureWithDescriptor:stencilDescriptor];
    MCRequire(stencil != nil, @"Stencil8 render target creation returned nil");
    MTLRenderPipelineDescriptor *maskDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    maskDescriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_vertex");
    maskDescriptor.fragmentFunction = MCFunction(context, @"mc_red_fragment");
    maskDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    maskDescriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    maskDescriptor.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    id<MTLRenderPipelineState> maskPipeline = MCRenderPipeline(context, maskDescriptor, @"stencil replace mask");
    MTLRenderPipelineDescriptor *testDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    testDescriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_vertex");
    testDescriptor.fragmentFunction = MCFunction(context, @"mc_red_fragment");
    testDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    testDescriptor.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    id<MTLRenderPipelineState> testPipeline = MCRenderPipeline(context, testDescriptor, @"stencil equal draw");

    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(color, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.stencilAttachment.texture = stencil;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setDepthStencilState:MCStencilState(context, MTLCompareFunctionAlways, MTLStencilOperationReplace)];
    [encoder setStencilReferenceValue:1];
    [encoder setRenderPipelineState:maskPipeline];
    MTLScissorRect leftHalf = {0, 0, 16, 32};
    [encoder setScissorRect:leftHalf];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder setDepthStencilState:MCStencilState(context, MTLCompareFunctionEqual, MTLStencilOperationKeep)];
    [encoder setRenderPipelineState:testPipeline];
    MTLScissorRect fullRect = {0, 0, width, height};
    [encoder setScissorRect:fullRect];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, color, 0, 0);
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t black[4] = {0, 0, 0, 255};
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    MCRequirePixel(readback, rowBytes, 8, 16, red, 0, @"stencil equal inside mask");
    MCRequirePixel(readback, rowBytes, 24, 16, black, 0, @"stencil equal outside mask");
}

static void MCTest18(MCContext *context) {
    const NSUInteger width = 32, height = 32;
    id<MTLTexture> first = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                     MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    id<MTLTexture> second = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                      MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_mrt_fragment");
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"two color attachments");
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = first;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[1].texture = second;
    pass.colorAttachments[1].loadAction = MTLLoadActionClear;
    pass.colorAttachments[1].storeAction = MTLStoreActionStore;
    pass.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *firstRead = MCReadPrivateTexture(context, first, 0, 0);
    NSData *secondRead = MCReadPrivateTexture(context, second, 0, 0);
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    NSUInteger rowBytes = MCAlign(width * 4, 256);
    MCRequirePixel(firstRead, rowBytes, 16, 16, red, 0, @"MRT attachment 0");
    MCRequirePixel(secondRead, rowBytes, 16, 16, green, 0, @"MRT attachment 1");
}

static void MCTest19(MCContext *context) {
    if (![context.device supportsTextureSampleCount:4]) {
        MCSkip(@"MTLDevice.supportsTextureSampleCount:4 is false");
    }
    const NSUInteger width = 32, height = 32;
    MTLTextureDescriptor *multisampleDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                      width:width height:height mipmapped:NO];
    multisampleDescriptor.textureType = MTLTextureType2DMultisample;
    multisampleDescriptor.sampleCount = 4;
    multisampleDescriptor.storageMode = MTLStorageModePrivate;
    multisampleDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> multisample = [context.device newTextureWithDescriptor:multisampleDescriptor];
    MCRequire(multisample != nil, @"4x multisample render target creation returned nil");
    MTLTextureDescriptor *resolveDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                               width:width * 2 height:height * 2 mipmapped:YES];
    resolveDescriptor.textureType = MTLTextureType2DArray;
    resolveDescriptor.arrayLength = 2;
    resolveDescriptor.storageMode = MTLStorageModePrivate;
    resolveDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> resolve = [context.device newTextureWithDescriptor:resolveDescriptor];
    MCRequire(resolve != nil, @"resolve array texture creation returned nil");
    id<MTLRenderPipelineState> pipeline = MCSimpleRenderPipeline(context, @"mc_fullscreen_vertex",
                                                                 @"mc_red_fragment", MTLPixelFormatRGBA8Unorm, 4);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = multisample;
    pass.colorAttachments[0].resolveTexture = resolve;
    pass.colorAttachments[0].resolveLevel = 1;
    pass.colorAttachments[0].resolveSlice = 1;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, resolve, 1, 1);
    const uint8_t red[4] = {255, 0, 0, 255};
    for (NSUInteger y = 0; y < height; y++) {
        for (NSUInteger x = 0; x < width; x++) {
            MCRequirePixel(readback, MCAlign(width * 4, 256), x, y, red, 0, @"4x resolve mip 1 slice 1");
        }
    }
}

static void MCTest20(MCContext *context) {
    const NSUInteger sourceWidth = 2, sourceHeight = 2;
    id<MTLTexture> source = MCTexture(context, MTLPixelFormatRGBA8Unorm, sourceWidth, sourceHeight,
                                      MTLStorageModeShared, MTLTextureUsageShaderRead, NO);
    const uint8_t quadrants[16] = {
        255, 0, 0, 255,  0, 255, 0, 255,
        0, 0, 255, 255,  255, 255, 255, 255,
    };
    [source replaceRegion:MTLRegionMake2D(0, 0, sourceWidth, sourceHeight) mipmapLevel:0
                withBytes:quadrants bytesPerRow:sourceWidth * 4];
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_uv_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_texture_sample_fragment");
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"nearest 2x2 sample");
    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [context.device newSamplerStateWithDescriptor:samplerDescriptor];
    MCRequire(sampler != nil, @"newSamplerStateWithDescriptor: returned nil");
    id<MTLTexture> destination = MCTexture(context, MTLPixelFormatRGBA8Unorm, 32, 32,
                                           MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(destination, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, destination, 0, 0);
    NSUInteger rowBytes = MCAlign(32 * 4, 256);
    MCRequirePixel(readback, rowBytes, 8, 8, quadrants, 0, @"nearest top-left");
    MCRequirePixel(readback, rowBytes, 24, 8, quadrants + 4, 0, @"nearest top-right");
    MCRequirePixel(readback, rowBytes, 8, 24, quadrants + 8, 0, @"nearest bottom-left");
    MCRequirePixel(readback, rowBytes, 24, 24, quadrants + 12, 0, @"nearest bottom-right");
}

static void MCTest21(MCContext *context) {
    const NSUInteger width = 16, height = 16;
    const uint8_t color[4] = {37, 91, 173, 255};
    id<MTLTexture> texture = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                       MTLStorageModeShared, MTLTextureUsageShaderRead, YES);
    NSMutableData *base = [NSMutableData dataWithLength:width * height * 4];
    uint8_t *bytes = static_cast<uint8_t *>(base.mutableBytes);
    for (NSUInteger index = 0; index < width * height; index++) std::memcpy(bytes + index * 4, color, 4);
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0
                withBytes:bytes bytesPerRow:width * 4];
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    [blit generateMipmapsForTexture:texture];
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    for (NSUInteger level = 0; level < texture.mipmapLevelCount; level++) {
        NSUInteger mipWidth = MAX((NSUInteger)1, width >> level);
        NSUInteger mipHeight = MAX((NSUInteger)1, height >> level);
        NSData *mip = MCReadSharedTexture(texture, 0, level, mipWidth * 4);
        const uint8_t *mipBytes = static_cast<const uint8_t *>(mip.bytes);
        for (NSUInteger y = 0; y < mipHeight; y++) {
            for (NSUInteger x = 0; x < mipWidth; x++) {
                const uint8_t *pixel = mipBytes + (y * mipWidth + x) * 4;
                MCRequire(std::memcmp(pixel, color, 4) == 0,
                          [NSString stringWithFormat:@"mipmap level %lu pixel (%lu,%lu) mismatch",
                           (unsigned long)level, (unsigned long)x, (unsigned long)y]);
            }
        }
    }
}

static void MCTest22(MCContext *context) {
    const NSUInteger width = 8, height = 8;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                           width:width height:height mipmapped:YES];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 2;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> arrayTexture = [context.device newTextureWithDescriptor:descriptor];
    MCRequire(arrayTexture != nil, @"2D array mipmapped texture creation returned nil");
    MCRequire(arrayTexture.mipmapLevelCount > 1, @"array texture has no nonzero mip level");
    const uint8_t color[4] = {19, 83, 207, 255};
    const NSUInteger mipWidth = 4, mipHeight = 4;
    NSMutableData *data = [NSMutableData dataWithLength:mipWidth * mipHeight * 4];
    uint8_t *bytes = static_cast<uint8_t *>(data.mutableBytes);
    for (NSUInteger index = 0; index < mipWidth * mipHeight; index++) std::memcpy(bytes + index * 4, color, 4);
    [arrayTexture replaceRegion:MTLRegionMake2D(0, 0, mipWidth, mipHeight)
                    mipmapLevel:1 slice:1 withBytes:bytes bytesPerRow:mipWidth * 4
                 bytesPerImage:mipWidth * mipHeight * 4];
    id<MTLTexture> view = [arrayTexture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                          textureType:MTLTextureType2D
                                                               levels:NSMakeRange(1, 1)
                                                               slices:NSMakeRange(1, 1)];
    MCRequire(view != nil, @"newTextureViewWithPixelFormat:textureType:levels:slices: returned nil");
    MCRequire(view.width == mipWidth && view.height == mipHeight && view.mipmapLevelCount == 1,
              @"texture view dimensions/levels did not select mip 1");
    NSData *readback = MCReadSharedTexture(view, 0, 0, mipWidth * 4);
    const uint8_t *readBytes = static_cast<const uint8_t *>(readback.bytes);
    for (NSUInteger index = 0; index < mipWidth * mipHeight; index++) {
        MCRequire(std::memcmp(readBytes + index * 4, color, 4) == 0,
                  [NSString stringWithFormat:@"texture view selected wrong mip/slice at texel %lu", (unsigned long)index]);
    }
}

static void MCTest23(MCContext *context) {
    const uint32_t count = 1024;
    id<MTLBuffer> counter = MCBuffer(context, sizeof(uint32_t), MTLResourceStorageModeShared);
    *static_cast<uint32_t *>(counter.contents) = 0;
    id<MTLComputePipelineState> pipeline = MCComputePipeline(context, @"mc_atomic_add");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    MCRequire(encoder != nil, @"computeCommandEncoder returned nil");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:counter offset:0 atIndex:0];
    [encoder setBytes:&count length:sizeof(count) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    MCRequire(*static_cast<uint32_t *>(counter.contents) == count, @"atomic add expected 1024");
}

static void MCTest24(MCContext *context) {
    const uint32_t count = 16;
    id<MTLBuffer> input = MCBuffer(context, count * sizeof(uint32_t), MTLResourceStorageModeShared);
    id<MTLBuffer> output = MCBuffer(context, sizeof(uint32_t), MTLResourceStorageModeShared);
    uint32_t *inputValues = static_cast<uint32_t *>(input.contents);
    for (uint32_t index = 0; index < count; index++) inputValues[index] = index;
    *static_cast<uint32_t *>(output.contents) = 0;
    id<MTLComputePipelineState> pipeline = MCComputePipeline(context, @"mc_threadgroup_sum");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    MCRequire(encoder != nil, @"computeCommandEncoder returned nil");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder setThreadgroupMemoryLength:count * sizeof(uint32_t) atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(count, 1, 1)];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    MCRequire(*static_cast<uint32_t *>(output.contents) == 120u,
              [NSString stringWithFormat:@"threadgroup sum expected 120 got %u", *static_cast<uint32_t *>(output.contents)]);
}

static void MCTest25(MCContext *context) {
    const uint32_t count = 257;
    id<MTLBuffer> input = MCBuffer(context, count * sizeof(float), MTLResourceStorageModeShared);
    id<MTLBuffer> output = MCBuffer(context, count * sizeof(float), MTLResourceStorageModeShared);
    float *inputValues = static_cast<float *>(input.contents);
    float *outputValues = static_cast<float *>(output.contents);
    for (uint32_t index = 0; index < count; index++) {
        inputValues[index] = (float)index * 0.25f - 17.0f;
        outputValues[index] = 0.0f;
    }
    for (float scale : {2.0f, 3.0f}) {
        MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
        [constants setConstantValue:&scale type:MTLDataTypeFloat atIndex:0];
        NSError *functionError = nil;
        id<MTLFunction> function = [context.library newFunctionWithName:@"mc_specialized_scale"
                                                        constantValues:constants error:&functionError];
        MCRequire(function != nil, [NSString stringWithFormat:@"function constant specialization failed: %@",
                                    functionError.localizedDescription != nil ? functionError.localizedDescription : @"no NSError"]);
        NSError *pipelineError = nil;
        id<MTLComputePipelineState> pipeline = [context.device newComputePipelineStateWithFunction:function
                                                                                              error:&pipelineError];
        MCRequire(pipeline != nil, [NSString stringWithFormat:@"specialized compute pipeline failed: %@",
                                    pipelineError.localizedDescription != nil ? pipelineError.localizedDescription : @"no NSError"]);
        id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
        MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        MCRequire(encoder != nil, @"computeCommandEncoder returned nil");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        MCWaitForCommandBuffer(commandBuffer);
        for (uint32_t index = 0; index < count; index++) {
            float expected = inputValues[index] * scale;
            MCRequire(std::fabs(outputValues[index] - expected) < 0.0001f,
                      [NSString stringWithFormat:@"scale %.0f index %u expected %.6f got %.6f",
                       scale, index, expected, outputValues[index]]);
        }
    }
}

static void MCTest26(MCContext *context) {
    const uint32_t count = 257;
    MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked;
    id<MTLBuffer> producer = MCBuffer(context, count * sizeof(uint32_t), options);
    id<MTLBuffer> consumer = MCBuffer(context, count * sizeof(uint32_t), options);
    std::memset(producer.contents, 0, producer.length);
    std::memset(consumer.contents, 0, consumer.length);
    id<MTLFence> fence = [context.device newFence];
    MCRequire(fence != nil, @"newFence returned nil");
    id<MTLComputePipelineState> fillPipeline = MCComputePipeline(context, @"mc_fill_sequence");
    id<MTLComputePipelineState> consumePipeline = MCComputePipeline(context, @"mc_consume_sequence");
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    id<MTLComputeCommandEncoder> producerEncoder = [commandBuffer computeCommandEncoder];
    MCRequire(producerEncoder != nil, @"producer computeCommandEncoder returned nil");
    [producerEncoder setComputePipelineState:fillPipeline];
    [producerEncoder setBuffer:producer offset:0 atIndex:0];
    [producerEncoder setBytes:&count length:sizeof(count) atIndex:1];
    [producerEncoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [producerEncoder updateFence:fence];
    [producerEncoder endEncoding];
    id<MTLComputeCommandEncoder> consumerEncoder = [commandBuffer computeCommandEncoder];
    MCRequire(consumerEncoder != nil, @"consumer computeCommandEncoder returned nil");
    [consumerEncoder waitForFence:fence];
    [consumerEncoder setComputePipelineState:consumePipeline];
    [consumerEncoder setBuffer:producer offset:0 atIndex:0];
    [consumerEncoder setBuffer:consumer offset:0 atIndex:1];
    [consumerEncoder setBytes:&count length:sizeof(count) atIndex:2];
    [consumerEncoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [consumerEncoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    const uint32_t *values = static_cast<const uint32_t *>(consumer.contents);
    for (uint32_t index = 0; index < count; index++) {
        uint32_t expected = (index * 5u + 9u) * 3u + 1u;
        MCRequire(values[index] == expected,
                  [NSString stringWithFormat:@"fence consumer index %u expected %u got %u", index, expected, values[index]]);
    }
}

static void MCTest27(MCContext *context) {
    const uint32_t count = 257;
    id<MTLCommandQueue> producerQueue = [context.device newCommandQueue];
    id<MTLCommandQueue> consumerQueue = [context.device newCommandQueue];
    MCRequire(producerQueue != nil && consumerQueue != nil, @"cross-queue newCommandQueue returned nil");
    id<MTLSharedEvent> event = [context.device newSharedEvent];
    MCRequire(event != nil, @"newSharedEvent returned nil");
    event.signaledValue = 0;
    id<MTLBuffer> producer = MCBuffer(context, count * sizeof(uint32_t), MTLResourceStorageModeShared);
    id<MTLBuffer> consumer = MCBuffer(context, count * sizeof(uint32_t), MTLResourceStorageModeShared);
    std::memset(producer.contents, 0, producer.length);
    std::memset(consumer.contents, 0, consumer.length);
    id<MTLComputePipelineState> fillPipeline = MCComputePipeline(context, @"mc_fill_sequence");
    id<MTLCommandBuffer> producerCommandBuffer = [producerQueue commandBuffer];
    id<MTLCommandBuffer> consumerCommandBuffer = [consumerQueue commandBuffer];
    MCRequire(producerCommandBuffer != nil && consumerCommandBuffer != nil, @"cross-queue commandBuffer returned nil");
    id<MTLComputeCommandEncoder> producerEncoder = [producerCommandBuffer computeCommandEncoder];
    MCRequire(producerEncoder != nil, @"producer computeCommandEncoder returned nil");
    [producerEncoder setComputePipelineState:fillPipeline];
    [producerEncoder setBuffer:producer offset:0 atIndex:0];
    [producerEncoder setBytes:&count length:sizeof(count) atIndex:1];
    [producerEncoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [producerEncoder endEncoding];
    [producerCommandBuffer encodeSignalEvent:event value:1];

    [consumerCommandBuffer encodeWaitForEvent:event value:1];
    id<MTLBlitCommandEncoder> consumerEncoder = [consumerCommandBuffer blitCommandEncoder];
    MCRequire(consumerEncoder != nil, @"consumer blitCommandEncoder returned nil");
    [consumerEncoder copyFromBuffer:producer sourceOffset:0 toBuffer:consumer destinationOffset:0 size:producer.length];
    [consumerEncoder endEncoding];
    // Submit the dependent work first so queue submission order cannot provide
    // the dependency which this test attributes to the shared event.
    MCWaitForCommandBuffers(@[consumerCommandBuffer, producerCommandBuffer], YES);
    MCRequire(event.signaledValue >= 1, @"GPU shared-event signal did not become visible to the CPU");
    const uint32_t *values = static_cast<const uint32_t *>(consumer.contents);
    for (uint32_t index = 0; index < count; index++) {
        uint32_t expected = index * 5u + 9u;
        MCRequire(values[index] == expected,
                  [NSString stringWithFormat:@"shared event copy index %u expected %u got %u", index, expected, values[index]]);
    }
}

static void MCTest28(MCContext *context) {
    const NSUInteger width = 32, height = 32, rowBytes = 256;
    const uint32_t bgraPixelFormat = 0x42475241u;
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @(width),
        (__bridge NSString *)kIOSurfaceHeight: @(height),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(rowBytes),
        (__bridge NSString *)kIOSurfaceAllocSize: @(rowBytes * height),
        (__bridge NSString *)kIOSurfacePixelFormat: @(bgraPixelFormat),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    MCRequire(surface != nullptr, @"IOSurfaceCreate returned null");
    BOOL surfaceLocked = NO;
    @try {
    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                 width:width height:height mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [context.device newTextureWithDescriptor:textureDescriptor iosurface:surface plane:0];
    MCRequire(texture != nil, @"newTextureWithDescriptor:iosurface:plane: returned nil");
    id<MTLRenderPipelineState> pipeline = MCSimpleRenderPipeline(context, @"mc_fullscreen_vertex",
                                                                 @"mc_bgra_fragment", MTLPixelFormatBGRA8Unorm, 1);
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(texture, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *textureReadback = MCReadPrivateTexture(context, texture, 0, 0);
    const uint8_t expectedBGRA[4] = {0, 64, 255, 255};
    MCRequirePixel(textureReadback, rowBytes, 16, 16, expectedBGRA, 1, @"IOSurface-backed texture readback");
    kern_return_t lockResult = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    surfaceLocked = lockResult == KERN_SUCCESS;
    MCRequire(lockResult == KERN_SUCCESS, [NSString stringWithFormat:@"IOSurfaceLock returned %d", lockResult]);
    const uint8_t *surfaceBytes = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surface));
    MCRequire(surfaceBytes != nullptr, @"IOSurfaceGetBaseAddress returned null");
    MCRequirePixel([NSData dataWithBytesNoCopy:(void *)(surfaceBytes + 16 * rowBytes) length:rowBytes freeWhenDone:NO],
                   rowBytes, 16, 0, expectedBGRA, 1, @"IOSurface backing bytes");
    kern_return_t unlockResult = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    MCRequire(unlockResult == KERN_SUCCESS, [NSString stringWithFormat:@"IOSurfaceUnlock returned %d", unlockResult]);
    surfaceLocked = NO;
    } @finally {
        if (surfaceLocked) IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
        CFRelease(surface);
    }
}

static void MCTest29(MCContext *context) {
    const NSUInteger surfaceWidth = 256, surfaceHeight = 64, rowBytes = 1024;
    const uint32_t bgraPixelFormat = 0x42475241u;
    const NSUInteger rotations[] = {1, 2, 3, 4};
    const uint8_t expectedBGRA[4][4] = {
        {0, 0, 255, 255},   // red
        {0, 255, 0, 255},   // green
        {255, 0, 0, 255},   // blue
        {255, 255, 255, 255}, // white
    };

    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                 width:surfaceWidth
                                                                                                height:surfaceHeight
                                                                                             mipmapped:NO];
    MCRequire(textureDescriptor != nil, @"texture2DDescriptorWithPixelFormat returned nil");
    SEL setRotationSelector = NSSelectorFromString(@"setRotation:");
    SEL setFramebufferOnlySelector = NSSelectorFromString(@"setFramebufferOnly:");
    SEL setIsDrawableSelector = NSSelectorFromString(@"setIsDrawable:");
    MCRequire([textureDescriptor respondsToSelector:setRotationSelector], @"MTLTextureDescriptor does not respond to setRotation:");
    MCRequire([textureDescriptor respondsToSelector:setFramebufferOnlySelector], @"MTLTextureDescriptor does not respond to setFramebufferOnly:");
    MCRequire([textureDescriptor respondsToSelector:setIsDrawableSelector], @"MTLTextureDescriptor does not respond to setIsDrawable:");

    SEL rotationSelector = NSSelectorFromString(@"rotation");
    SEL framebufferOnlySelector = NSSelectorFromString(@"isFramebufferOnly");
    SEL isDrawableSelector = NSSelectorFromString(@"isDrawable");
    id<MTLRenderPipelineState> pipeline = MCSimpleRenderPipeline(context, @"mc_fullscreen_vertex",
                                                                 @"mc_benchmark_color_fragment",
                                                                 MTLPixelFormatBGRA8Unorm, 1);

    for (NSUInteger rotationIndex = 0; rotationIndex < sizeof(rotations) / sizeof(rotations[0]); rotationIndex++) {
        NSUInteger rotation = rotations[rotationIndex];
        NSUInteger logicalWidth = rotation == 2 ? surfaceWidth : surfaceHeight;
        NSUInteger logicalHeight = rotation == 2 ? surfaceHeight : surfaceWidth;
        NSDictionary *properties = @{
            (__bridge NSString *)kIOSurfaceWidth: @(surfaceWidth),
            (__bridge NSString *)kIOSurfaceHeight: @(surfaceHeight),
            (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
            (__bridge NSString *)kIOSurfaceBytesPerRow: @(rowBytes),
            (__bridge NSString *)kIOSurfaceAllocSize: @(rowBytes * surfaceHeight),
            (__bridge NSString *)kIOSurfacePixelFormat: @(bgraPixelFormat),
        };
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        MCRequire(surface != nullptr, [NSString stringWithFormat:@"IOSurfaceCreate returned null for rotation %lu",
                                       (unsigned long)rotation]);
        BOOL surfaceLocked = NO;
        @try {
            textureDescriptor.width = logicalWidth;
            textureDescriptor.height = logicalHeight;
            textureDescriptor.storageMode = MTLStorageModeShared;
            textureDescriptor.usage = MTLTextureUsageRenderTarget;
            ((void (*)(id, SEL, NSUInteger))objc_msgSend)(textureDescriptor, setRotationSelector, rotation);
            ((void (*)(id, SEL, BOOL))objc_msgSend)(textureDescriptor, setFramebufferOnlySelector, YES);
            ((void (*)(id, SEL, BOOL))objc_msgSend)(textureDescriptor, setIsDrawableSelector, YES);

            id<MTLTexture> texture = [context.device newTextureWithDescriptor:textureDescriptor iosurface:surface plane:0];
            MCRequire(texture != nil, [NSString stringWithFormat:@"newTextureWithDescriptor:iosurface:plane: returned nil for rotation %lu",
                                       (unsigned long)rotation]);
            MCRequire([texture respondsToSelector:rotationSelector], @"IOSurface texture does not respond to rotation");
            NSUInteger actualRotation = ((NSUInteger (*)(id, SEL))objc_msgSend)(texture, rotationSelector);
            MCRequire(actualRotation == rotation,
                      [NSString stringWithFormat:@"IOSurface texture rotation expected %lu got %lu",
                       (unsigned long)rotation, (unsigned long)actualRotation]);
            MCRequire([texture respondsToSelector:framebufferOnlySelector], @"IOSurface texture does not respond to isFramebufferOnly");
            BOOL framebufferOnly = ((BOOL (*)(id, SEL))objc_msgSend)(texture, framebufferOnlySelector);
            MCRequire(framebufferOnly, @"IOSurface texture is not framebuffer-only");
            if ([texture respondsToSelector:isDrawableSelector]) {
                BOOL isDrawable = ((BOOL (*)(id, SEL))objc_msgSend)(texture, isDrawableSelector);
                MCRequire(isDrawable, @"IOSurface texture is not drawable");
            }

            id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
            MCRequire(commandBuffer != nil, @"commandQueue commandBuffer returned nil");
            MTLRenderPassDescriptor *pass = MCRenderPass(texture, MTLLoadActionClear, MTLStoreActionStore);
            MCRequire(pass != nil, @"renderPassDescriptor returned nil");
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
            MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
            [encoder setRenderPipelineState:pipeline];
            const MTLScissorRect scissorRects[] = {
                {0, 0, logicalWidth / 2, logicalHeight / 2},
                {logicalWidth / 2, 0, logicalWidth / 2, logicalHeight / 2},
                {0, logicalHeight / 2, logicalWidth / 2, logicalHeight / 2},
                {logicalWidth / 2, logicalHeight / 2, logicalWidth / 2, logicalHeight / 2},
            };
            const simd_float4 colors[] = {
                {1.0f, 0.0f, 0.0f, 1.0f},
                {0.0f, 1.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, 1.0f, 1.0f},
                {1.0f, 1.0f, 1.0f, 1.0f},
            };
            for (NSUInteger quadrant = 0; quadrant < 4; quadrant++) {
                [encoder setScissorRect:scissorRects[quadrant]];
                [encoder setFragmentBytes:&colors[quadrant] length:sizeof(colors[quadrant]) atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [encoder endEncoding];
            MCWaitForCommandBuffer(commandBuffer);

            kern_return_t lockResult = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
            surfaceLocked = lockResult == KERN_SUCCESS;
            MCRequire(lockResult == KERN_SUCCESS,
                      [NSString stringWithFormat:@"IOSurfaceLock returned %d for rotation %lu",
                       lockResult, (unsigned long)rotation]);
            MCRequire(IOSurfaceGetWidth(surface) == surfaceWidth && IOSurfaceGetHeight(surface) == surfaceHeight,
                      @"IOSurface dimensions differ from the requested physical dimensions");
            MCRequire(IOSurfaceGetBytesPerRow(surface) == rowBytes,
                      @"IOSurface bytes per row differs from the requested row stride");
            const uint8_t *surfaceBytes = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surface));
            MCRequire(surfaceBytes != nullptr, @"IOSurfaceGetBaseAddress returned null");

            for (NSUInteger logicalY = 0; logicalY < logicalHeight; logicalY++) {
                for (NSUInteger logicalX = 0; logicalX < logicalWidth; logicalX++) {
                    NSUInteger storedX = 0, storedY = 0;
                    switch (rotation) {
                        case 1:
                        case 4:
                            storedX = logicalHeight - 1 - logicalY;
                            storedY = logicalX;
                            break;
                        case 2:
                            storedX = logicalWidth - 1 - logicalX;
                            storedY = logicalHeight - 1 - logicalY;
                            break;
                        case 3:
                            storedX = logicalY;
                            storedY = logicalWidth - 1 - logicalX;
                            break;
                        default:
                            MCRequire(NO, @"unexpected IOSurface rotation");
                    }
                    MCRequire(storedX < surfaceWidth && storedY < surfaceHeight,
                              [NSString stringWithFormat:@"rotation %lu mapped logical pixel (%lu,%lu) outside the IOSurface",
                               (unsigned long)rotation, (unsigned long)logicalX, (unsigned long)logicalY]);
                    NSUInteger quadrant = (logicalY >= logicalHeight / 2 ? 2 : 0) +
                                          (logicalX >= logicalWidth / 2 ? 1 : 0);
                    const uint8_t *actual = surfaceBytes + storedY * rowBytes + storedX * 4;
                    for (NSUInteger channel = 0; channel < 4; channel++) {
                        if (actual[channel] != expectedBGRA[quadrant][channel]) MCRequire(NO,
                                  [NSString stringWithFormat:@"rotation %lu logical (%lu,%lu) stored (%lu,%lu) channel %lu expected %u got %u",
                                   (unsigned long)rotation, (unsigned long)logicalX, (unsigned long)logicalY,
                                   (unsigned long)storedX, (unsigned long)storedY, (unsigned long)channel,
                                   (unsigned)expectedBGRA[quadrant][channel], (unsigned)actual[channel]]);
                    }
                }
            }

            kern_return_t unlockResult = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
            if (unlockResult == KERN_SUCCESS) surfaceLocked = NO;
            MCRequire(unlockResult == KERN_SUCCESS,
                      [NSString stringWithFormat:@"IOSurfaceUnlock returned %d for rotation %lu",
                       unlockResult, (unsigned long)rotation]);
        } @finally {
            if (surfaceLocked) IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
            CFRelease(surface);
        }
    }
}

// The rows of a small CPU-drawn image as the compositor gets them: a shared-memory bitmap whose row pitch is the
// width rounded to the linear alignment, rarely a multiple of 64 bytes (text labels, the status bar clock). Each
// width is copied into a private texture by a blit at that pitch, and viewed as a texture over the buffer at that
// pitch and sampled 1:1; both must give back every texel. A row read at another pitch shows as a row shift.
static const NSUInteger MCUnalignedRowWidths[] = {7, 13, 61, 77, 100, 333};
static const NSUInteger MCUnalignedRowHeight = 9;

static NSUInteger MCUnalignedRowBytes(NSUInteger width, NSUInteger alignment) {
    NSUInteger rowBytes = MCAlign(width * 4, alignment);
    return rowBytes % 64 ? rowBytes : rowBytes + alignment;
}

static void MCRequireUnalignedRows(NSData *readback, NSUInteger readRowBytes, const uint8_t *source,
                                   NSUInteger rowBytes, NSUInteger width, NSString *path) {
    const uint8_t *actual = static_cast<const uint8_t *>(readback.bytes);
    for (NSUInteger y = 0; y < MCUnalignedRowHeight; y++) {
        for (NSUInteger x = 0; x < width * 4; x++) {
            uint8_t want = source[y * rowBytes + x], got = actual[y * readRowBytes + x];
            MCRequire(got == want, [NSString stringWithFormat:@"%@ width %lu rows %lu bytes apart: row %lu byte %lu "
                                   @"expected %u got %u", path, (unsigned long)width, (unsigned long)rowBytes,
                                   (unsigned long)y, (unsigned long)x, (unsigned)want, (unsigned)got]);
        }
    }
}

static void MCTest30(MCContext *context) {
    NSUInteger alignment = [context.device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm];
    MCRequire(alignment > 0 && alignment < 64 && !(alignment & (alignment - 1)),
              [NSString stringWithFormat:@"minimumLinearTextureAlignmentForPixelFormat: %lu leaves no row pitch "
                                         @"that is not a multiple of 64", (unsigned long)alignment]);
    alignment = MAX(alignment, (NSUInteger)4);
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_uv_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_texture_sample_fragment");
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"unaligned rows 1:1 sample");
    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [context.device newSamplerStateWithDescriptor:samplerDescriptor];
    MCRequire(sampler != nil, @"newSamplerStateWithDescriptor: returned nil");
    for (NSUInteger width : MCUnalignedRowWidths) {
        const NSUInteger height = MCUnalignedRowHeight, rowBytes = MCUnalignedRowBytes(width, alignment);
        id<MTLBuffer> buffer = MCBuffer(context, rowBytes * height, MTLResourceStorageModeShared);
        uint8_t *bytes = static_cast<uint8_t *>(buffer.contents);
        std::memset(bytes, 0xee, buffer.length);
        for (NSUInteger y = 0; y < height; y++) {
            for (NSUInteger x = 0; x < width; x++) {
                uint8_t *pixel = bytes + y * rowBytes + x * 4;
                pixel[0] = (uint8_t)(x * 7u + y * 3u);
                pixel[1] = (uint8_t)(y * 37u + 1u);
                pixel[2] = (uint8_t)(x ^ (y * 11u));
                pixel[3] = 255;
            }
        }
        id<MTLTexture> copied = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height, MTLStorageModePrivate,
                                          MTLTextureUsageShaderRead, NO);
        id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
        MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        MCRequire(blit != nil, @"blitCommandEncoder returned nil");
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:rowBytes * height
                  sourceSize:MTLSizeMake(width, height, 1) toTexture:copied destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        MCWaitForCommandBuffer(commandBuffer);
        MCRequireUnalignedRows(MCReadPrivateTexture(context, copied, 0, 0), MCAlign(width * 4, 256), bytes, rowBytes,
                               width, @"buffer-to-texture blit");

        MTLTextureDescriptor *viewDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height
                                                           mipmapped:NO];
        viewDescriptor.storageMode = MTLStorageModeShared;
        viewDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> view = [buffer newTextureWithDescriptor:viewDescriptor offset:0 bytesPerRow:rowBytes];
        MCRequire(view != nil, [NSString stringWithFormat:@"newTextureWithDescriptor:offset:bytesPerRow:%lu returned nil "
                                                          @"for width %lu", (unsigned long)rowBytes, (unsigned long)width]);
        id<MTLTexture> drawn = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height, MTLStorageModePrivate,
                                         MTLTextureUsageRenderTarget, NO);
        commandBuffer = [context.queue commandBuffer];
        MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:MCRenderPass(drawn, MTLLoadActionDontCare, MTLStoreActionStore)];
        MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
        [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentTexture:view atIndex:0];
        [encoder setFragmentSamplerState:sampler atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        MCWaitForCommandBuffer(commandBuffer);
        MCRequireUnalignedRows(MCReadPrivateTexture(context, drawn, 0, 0), MCAlign(width * 4, 256), bytes, rowBytes,
                               width, @"texture over the buffer, sampled");
    }
}

// CoreAnimation hands the render server each CPU-drawn layer as the client's shared memory wrapped in a no-copy buffer,
// rewritten in place for the next frame. Whatever the CPU last wrote there must be what the GPU reads, even while those
// lines are still in the CPU's caches: a stale line mixes the old frame's rows into the new one. Each round rewrites the
// whole region with another pattern and copies it out on the GPU at once; every 64-byte line must hold the new pattern.
static void MCTest31(MCContext *context) {
    const NSUInteger length = 256 * 1024, rounds = 8, line = 64;
    void *memory = nullptr;
    MCRequire(posix_memalign(&memory, (size_t)getpagesize(), length) == 0, @"posix_memalign failed");
    std::memset(memory, 0, length);
    id<MTLBuffer> wrapped = [context.device newBufferWithBytesNoCopy:memory length:length options:MTLResourceStorageModeShared
                                                         deallocator:^(void *pointer, NSUInteger) { free(pointer); }];
    if (!wrapped) free(memory);
    MCRequire(wrapped != nil, @"newBufferWithBytesNoCopy:length:options:deallocator: returned nil");
    id<MTLBuffer> copy = MCBuffer(context, length, MTLResourceStorageModeShared);
    uint32_t *words = static_cast<uint32_t *>(memory);
    for (NSUInteger round = 0; round < rounds; round++) {
        for (NSUInteger i = 0; i < length / 4; i++) words[i] = (uint32_t)(round * 0x01010101u) ^ (uint32_t)(i * 2654435761u);
        id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
        MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        MCRequire(blit != nil, @"blitCommandEncoder returned nil");
        [blit copyFromBuffer:wrapped sourceOffset:0 toBuffer:copy destinationOffset:0 size:length];
        [blit endEncoding];
        MCWaitForCommandBuffer(commandBuffer);
        const uint8_t *want = static_cast<const uint8_t *>(memory), *got = static_cast<const uint8_t *>(copy.contents);
        NSUInteger stale = 0, first = NSNotFound;
        for (NSUInteger at = 0; at < length; at += line) {
            if (std::memcmp(want + at, got + at, line)) { stale++; if (first == NSNotFound) first = at; }
        }
        MCRequire(stale == 0, [NSString stringWithFormat:@"round %lu: the GPU read %lu of %lu cache lines the CPU had just "
                                                         @"rewritten as they were before, the first at byte %lu",
                               (unsigned long)round, (unsigned long)stale, (unsigned long)(length / line), (unsigned long)first]);
    }
}

// A client's freed backing store is often replaced by a new one at the same address: other pages behind the same virtual
// addresses. A GPU import remembered by address (host pages, an IOSurface mapping) would keep reading the old pages, which
// draws another layer's text in a label's place. Each round maps new pages at the address the previous round used (no-copy
// buffers, MAP_FIXED), or makes a new IOSurface of the same size (which the kernel tends to map where the last one was), fills
// them with that round's pattern and copies them out on the GPU; every round must read its own pattern.
static void MCTest32(MCContext *context) {
    const NSUInteger length = 64 * 1024, rounds = 6;
    id<MTLBuffer> copy = MCBuffer(context, length, MTLResourceStorageModeShared);
    auto check = [&](const void *want, NSString *what, NSUInteger round) {
        NSUInteger stale = 0, first = NSNotFound;
        for (NSUInteger at = 0; at < length; at += 64) {
            if (std::memcmp(static_cast<const uint8_t *>(want) + at, static_cast<const uint8_t *>(copy.contents) + at, 64)) {
                stale++; if (first == NSNotFound) first = at;
            }
        }
        MCRequire(stale == 0, [NSString stringWithFormat:@"%@ round %lu: %lu of %lu lines are not this round's, the first at byte %lu",
                               what, (unsigned long)round, (unsigned long)stale, (unsigned long)(length / 64), (unsigned long)first]);
    };
    auto fill = [](uint32_t *words, NSUInteger count, NSUInteger round) {
        for (NSUInteger i = 0; i < count; i++) words[i] = (uint32_t)((round + 1) * 0x9e3779b9u) ^ (uint32_t)(i * 2246822519u);
    };
    void *address = nullptr;
    for (NSUInteger round = 0; round < rounds; round++) {
        void *memory = mmap(address, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | (address ? MAP_FIXED : 0), -1, 0);
        MCRequire(memory != MAP_FAILED, @"mmap failed");
        MCRequire(!address || memory == address, @"mmap MAP_FIXED did not map the same address");
        address = memory;
        fill(static_cast<uint32_t *>(memory), length / 4, round);
        @autoreleasepool {
            id<MTLBuffer> wrapped = [context.device newBufferWithBytesNoCopy:memory length:length
                                                                     options:MTLResourceStorageModeShared deallocator:nil];
            MCRequire(wrapped != nil, @"newBufferWithBytesNoCopy:length:options:deallocator: returned nil");
            id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
            [blit copyFromBuffer:wrapped sourceOffset:0 toBuffer:copy destinationOffset:0 size:length];
            [blit endEncoding];
            MCWaitForCommandBuffer(commandBuffer);
            check(memory, @"no-copy buffer over new pages at the same address", round);
        }
        // The buffer is gone; the next round maps other pages at this address (MAP_FIXED replaces these).
    }
    munmap(address, length);

    const NSUInteger side = 128;   // 128 x 128 BGRA, 512-byte rows: 64 KiB
    for (NSUInteger round = 0; round < rounds; round++) {
        NSDictionary *properties = @{(id)kIOSurfaceWidth: @(side), (id)kIOSurfaceHeight: @(side), (id)kIOSurfaceBytesPerElement: @4,
                                     (id)kIOSurfaceBytesPerRow: @(side * 4), (id)kIOSurfacePixelFormat: @(0x42475241)};
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        MCRequire(surface != nullptr, @"IOSurfaceCreate returned null");
        @try {
            MCRequire(IOSurfaceLock(surface, 0, nullptr) == KERN_SUCCESS, @"IOSurfaceLock failed");
            fill(static_cast<uint32_t *>(IOSurfaceGetBaseAddress(surface)), length / 4, round);
            IOSurfaceUnlock(surface, 0, nullptr);
            @autoreleasepool {
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                      width:side height:side mipmapped:NO];
                descriptor.usage = MTLTextureUsageShaderRead;
                id<MTLTexture> texture = [context.device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
                MCRequire(texture != nil, @"newTextureWithDescriptor:iosurface:plane: returned nil");
                id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(side, side, 1) toBuffer:copy destinationOffset:0
               destinationBytesPerRow:side * 4 destinationBytesPerImage:length];
                [blit endEncoding];
                MCWaitForCommandBuffer(commandBuffer);
                check(IOSurfaceGetBaseAddress(surface), @"new IOSurface of the same size", round);
            }
        } @finally {
            CFRelease(surface);
        }
    }
}

// The compositor packs small layer images into a large texture it then samples from (an atlas): rectangles of odd size at
// odd places, uploaded from buffers whose rows are not 64-byte aligned, or copied from other textures. A rectangle written at
// the wrong place or with the wrong rows would show another layer's text, or garbled rows, inside a label. A 1024x512
// private render-target texture (the driver may tile or compress it) is cleared, then each rectangle is written by a blit
// from a buffer and others by a blit from a texture; every texel of the whole texture is checked.
static void MCTest33(MCContext *context) {
    const NSUInteger width = 1024, height = 512;
    struct Rect { NSUInteger x, y, w, h; };
    const Rect fromBuffer[] = {{1, 1, 7, 9}, {13, 5, 61, 17}, {100, 37, 77, 20}, {333, 101, 333, 25}, {700, 200, 13, 40},
                               {5, 300, 100, 9}, {1000, 3, 23, 30}, {511, 480, 129, 31}};
    const Rect fromTexture[] = {{40, 140, 61, 13}, {250, 250, 200, 21}, {900, 333, 99, 17}};
    id<MTLTexture> atlas = MCTexture(context, MTLPixelFormatBGRA8Unorm, width, height, MTLStorageModePrivate,
                                     MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, NO);
    std::vector<uint8_t> want(width * height * 4);
    const uint8_t clear[4] = {0x10, 0x20, 0x30, 0xff};   // BGRA bytes of the clear color below
    for (NSUInteger i = 0; i < width * height; i++) std::memcpy(&want[i * 4], clear, 4);
    auto texel = [](NSUInteger rect, NSUInteger x, NSUInteger y, uint8_t out[4]) {
        out[0] = (uint8_t)(x * 5u + rect * 31u); out[1] = (uint8_t)(y * 11u + 7u); out[2] = (uint8_t)((x ^ y) + rect); out[3] = 255;
    };
    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
    MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
    MTLRenderPassDescriptor *pass = MCRenderPass(atlas, MTLLoadActionClear, MTLStoreActionStore);
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0x30 / 255.0, 0x20 / 255.0, 0x10 / 255.0, 1);
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
    [encoder endEncoding];
    NSUInteger alignment = MAX([context.device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm], (NSUInteger)4);
    NSMutableArray *keep = [NSMutableArray array];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    MCRequire(blit != nil, @"blitCommandEncoder returned nil");
    NSUInteger index = 0;
    for (const Rect &r : fromBuffer) {
        const NSUInteger rowBytes = MCUnalignedRowBytes(r.w, alignment);
        id<MTLBuffer> source = MCBuffer(context, rowBytes * r.h, MTLResourceStorageModeShared);
        [keep addObject:source];
        uint8_t *bytes = static_cast<uint8_t *>(source.contents);
        for (NSUInteger y = 0; y < r.h; y++)
            for (NSUInteger x = 0; x < r.w; x++) {
                texel(index, x, y, bytes + y * rowBytes + x * 4);
                std::memcpy(&want[((r.y + y) * width + r.x + x) * 4], bytes + y * rowBytes + x * 4, 4);
            }
        [blit copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:rowBytes * r.h
                  sourceSize:MTLSizeMake(r.w, r.h, 1) toTexture:atlas destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(r.x, r.y, 0)];
        index++;
    }
    for (const Rect &r : fromTexture) {
        // The source has a margin, so the copy starts inside it, as a sub-image of a larger image does.
        id<MTLTexture> source = MCTexture(context, MTLPixelFormatBGRA8Unorm, r.w + 3, r.h + 2, MTLStorageModeShared,
                                          MTLTextureUsageShaderRead, NO);
        [keep addObject:source];
        std::vector<uint8_t> pixels((r.w + 3) * (r.h + 2) * 4, 0x77);
        for (NSUInteger y = 0; y < r.h; y++)
            for (NSUInteger x = 0; x < r.w; x++) {
                uint8_t *p = &pixels[((y + 1) * (r.w + 3) + x + 2) * 4];
                texel(index, x, y, p);
                std::memcpy(&want[((r.y + y) * width + r.x + x) * 4], p, 4);
            }
        [source replaceRegion:MTLRegionMake2D(0, 0, r.w + 3, r.h + 2) mipmapLevel:0 withBytes:pixels.data()
                  bytesPerRow:(r.w + 3) * 4];
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(2, 1, 0)
                   sourceSize:MTLSizeMake(r.w, r.h, 1) toTexture:atlas destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(r.x, r.y, 0)];
        index++;
    }
    [blit endEncoding];
    MCWaitForCommandBuffer(commandBuffer);
    NSData *readback = MCReadPrivateTexture(context, atlas, 0, 0);
    const NSUInteger readRow = MCAlign(width * 4, 256);
    const uint8_t *got = static_cast<const uint8_t *>(readback.bytes);
    for (NSUInteger y = 0; y < height; y++)
        for (NSUInteger x = 0; x < width; x++) {
            const uint8_t *g = got + y * readRow + x * 4, *w = &want[(y * width + x) * 4];
            MCRequire(!std::memcmp(g, w, 4), [NSString stringWithFormat:@"atlas texel (%lu,%lu) expected %02x%02x%02x%02x got "
                                                                        @"%02x%02x%02x%02x", (unsigned long)x, (unsigned long)y,
                                               w[0], w[1], w[2], w[3], g[0], g[1], g[2], g[3]]);
        }
}

// The compositor writes a layer image into a texture and draws from it, frame after frame, without waiting for the GPU in
// between: Metal orders a command buffer's write to a tracked texture after the reads of the command buffers before it on the
// queue, and its draws after its write. Sixteen command buffers are committed back to back; each writes its round's colour
// into one texture they all share (a blit from a buffer, or a render pass that clears it) and then draws that texture 1:1 into
// an output of its own. Every output must hold its own round's colour: a write that overtakes an earlier round's draw, or a
// draw that runs before its round's write, shows another round's colour (another layer's image) in it.
static void MCTest34(MCContext *context) {
    const NSUInteger width = 256, height = 64, rounds = 16;
    id<MTLTexture> sharedTexture = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height, MTLStorageModePrivate,
                                             MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, NO);
    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = MCFunction(context, @"mc_fullscreen_uv_vertex");
    descriptor.fragmentFunction = MCFunction(context, @"mc_texture_sample_fragment");
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = MCRenderPipeline(context, descriptor, @"in-flight 1:1 sample");
    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> sampler = [context.device newSamplerStateWithDescriptor:samplerDescriptor];
    MCRequire(sampler != nil, @"newSamplerStateWithDescriptor: returned nil");
    auto colour = [](NSUInteger mode, NSUInteger round, uint8_t out[4]) {
        out[0] = (uint8_t)(17u * round + 5u + 100u * mode); out[1] = (uint8_t)(255u - 13u * round);
        out[2] = (uint8_t)(31u * round + 7u * mode); out[3] = 255;
    };
    for (NSUInteger mode = 0; mode < 2; mode++) {
        NSMutableArray<id<MTLCommandBuffer>> *commandBuffers = [NSMutableArray array];
        NSMutableArray *outputs = [NSMutableArray array], *keep = [NSMutableArray array];
        for (NSUInteger round = 0; round < rounds; round++) {
            uint8_t c[4]; colour(mode, round, c);
            id<MTLTexture> output = MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height, MTLStorageModePrivate,
                                              MTLTextureUsageRenderTarget, NO);
            [outputs addObject:output];
            id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
            MCRequire(commandBuffer != nil, @"commandBuffer returned nil");
            if (mode == 0) {
                id<MTLBuffer> source = MCBuffer(context, width * height * 4, MTLResourceStorageModeShared);
                [keep addObject:source];
                uint8_t *bytes = static_cast<uint8_t *>(source.contents);
                for (NSUInteger i = 0; i < width * height; i++) std::memcpy(bytes + i * 4, c, 4);
                id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                [blit copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:width * 4 sourceBytesPerImage:width * height * 4
                          sourceSize:MTLSizeMake(width, height, 1) toTexture:sharedTexture destinationSlice:0 destinationLevel:0
                   destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
            } else {
                MTLRenderPassDescriptor *write = MCRenderPass(sharedTexture, MTLLoadActionClear, MTLStoreActionStore);
                write.colorAttachments[0].clearColor = MTLClearColorMake(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, 1);
                [[commandBuffer renderCommandEncoderWithDescriptor:write] endEncoding];
            }
            id<MTLRenderCommandEncoder> encoder =
                [commandBuffer renderCommandEncoderWithDescriptor:MCRenderPass(output, MTLLoadActionDontCare, MTLStoreActionStore)];
            MCRequire(encoder != nil, @"renderCommandEncoderWithDescriptor: returned nil");
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:sharedTexture atIndex:0];
            [encoder setFragmentSamplerState:sampler atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [commandBuffers addObject:commandBuffer];
        }
        MCWaitForCommandBuffers(commandBuffers, YES);
        for (NSUInteger round = 0; round < rounds; round++) {
            uint8_t c[4]; colour(mode, round, c);
            NSData *readback = MCReadPrivateTexture(context, outputs[round], 0, 0);
            const NSUInteger row = MCAlign(width * 4, 256);
            NSUInteger wrong = 0, other = NSNotFound;
            for (NSUInteger y = 0; y < height; y++)
                for (NSUInteger x = 0; x < width; x++) {
                    const uint8_t *g = MCPixel(readback, row, x, y);
                    if (std::memcmp(g, c, 4)) {
                        wrong++;
                        for (NSUInteger r = 0; r < rounds && other == NSNotFound; r++) {
                            uint8_t o[4]; colour(mode, r, o); if (!std::memcmp(g, o, 4)) other = r;
                        }
                    }
                }
            MCRequire(wrong == 0, [NSString stringWithFormat:@"%@ round %lu: %lu of %lu texels are not this round's colour%@",
                                   mode ? @"render-pass write" : @"blit write", (unsigned long)round, (unsigned long)wrong,
                                   (unsigned long)(width * height),
                                   other == NSNotFound ? @"" : [NSString stringWithFormat:@" (some are round %lu's)", (unsigned long)other]]);
        }
    }
}

// A backing store the compositor does not show is marked volatile, and under memory pressure its pages are taken; when the
// client draws into it again it is made nonvolatile and gets other pages. A GPU import of the surface made before that still
// holds the old pages, and would show what the surface held before (another layer's image) or a mix of both. The surface is
// filled with A and read on the GPU through a texture of it; then it is emptied (kIOSurfacePurgeableEmpty), made nonvolatile
// again and filled with B on the CPU; the same texture and a new texture of the surface must both read B.
static void MCTest35(MCContext *context) {
    const NSUInteger side = 256, length = side * side * 4;   // 256 KiB: whole pages of their own
    NSDictionary *properties = @{(id)kIOSurfaceWidth: @(side), (id)kIOSurfaceHeight: @(side), (id)kIOSurfaceBytesPerElement: @4,
                                 (id)kIOSurfaceBytesPerRow: @(side * 4), (id)kIOSurfacePixelFormat: @(0x42475241)};
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    MCRequire(surface != nullptr, @"IOSurfaceCreate returned null");
    id<MTLBuffer> copy = MCBuffer(context, length, MTLResourceStorageModeShared);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:side height:side mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    auto fill = [&](uint32_t seed) {
        MCRequire(IOSurfaceLock(surface, 0, nullptr) == KERN_SUCCESS, @"IOSurfaceLock failed");
        uint32_t *words = static_cast<uint32_t *>(IOSurfaceGetBaseAddress(surface));
        for (NSUInteger i = 0; i < length / 4; i++) words[i] = seed ^ (uint32_t)(i * 2654435761u);
        IOSurfaceUnlock(surface, 0, nullptr);
    };
    auto read = [&](id<MTLTexture> texture, uint32_t seed, NSString *what) {
        id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(side, side, 1) toBuffer:copy destinationOffset:0
       destinationBytesPerRow:side * 4 destinationBytesPerImage:length];
        [blit endEncoding];
        MCWaitForCommandBuffer(commandBuffer);
        const uint32_t *got = static_cast<const uint32_t *>(copy.contents);
        NSUInteger wrong = 0, first = NSNotFound;
        for (NSUInteger i = 0; i < length / 4; i++)
            if (got[i] != (seed ^ (uint32_t)(i * 2654435761u))) { wrong++; if (first == NSNotFound) first = i * 4; }
        MCRequire(wrong == 0, [NSString stringWithFormat:@"%@: %lu of %lu words are not the surface's contents, the first at byte %lu",
                               what, (unsigned long)wrong, (unsigned long)(length / 4), (unsigned long)first]);
    };
    @try {
        fill(0x11111111u);
        id<MTLTexture> before = [context.device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        MCRequire(before != nil, @"newTextureWithDescriptor:iosurface:plane: returned nil");
        read(before, 0x11111111u, @"before the surface was emptied");
        uint32_t old = 0;
        MCRequire(IOSurfaceSetPurgeable(surface, kIOSurfacePurgeableEmpty, &old) == KERN_SUCCESS,
                  @"IOSurfaceSetPurgeable(kIOSurfacePurgeableEmpty) failed");
        MCRequire(IOSurfaceSetPurgeable(surface, kIOSurfacePurgeableNonVolatile, &old) == KERN_SUCCESS,
                  @"IOSurfaceSetPurgeable(kIOSurfacePurgeableNonVolatile) failed");
        fill(0x5a5a5a5au);
        read(before, 0x5a5a5a5au, @"the texture made before, after the surface was emptied and drawn again");
        id<MTLTexture> after = [context.device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        MCRequire(after != nil, @"newTextureWithDescriptor:iosurface:plane: returned nil");
        read(after, 0x5a5a5a5au, @"a new texture, after the surface was emptied and drawn again");
    } @finally {
        CFRelease(surface);
    }
}

typedef id<MTLCommandBuffer> (^MCBenchmarkCommandBlock)(NSUInteger slot, uint64_t serial);
typedef void (^MCBenchmarkOutputValidator)(NSArray<NSNumber *> *lastSerials);

static NSDictionary *MCDistribution(NSArray<NSNumber *> *numbers) {
    if (numbers.count == 0) return @{@"count": @0};
    NSArray<NSNumber *> *sorted = [numbers sortedArrayUsingComparator:^NSComparisonResult(NSNumber *left, NSNumber *right) {
        if (left.doubleValue < right.doubleValue) return NSOrderedAscending;
        if (left.doubleValue > right.doubleValue) return NSOrderedDescending;
        return NSOrderedSame;
    }];
    double sum = 0.0;
    for (NSNumber *number in numbers) sum += number.doubleValue;
    NSUInteger last = sorted.count - 1;
    NSUInteger p50 = (NSUInteger)std::floor((double)last * 0.50);
    NSUInteger p95 = (NSUInteger)std::floor((double)last * 0.95);
    return @{
        @"count": @(numbers.count),
        @"mean": @(sum / (double)numbers.count),
        @"p50": sorted[p50],
        @"p95": sorted[p95],
        @"max": sorted.lastObject,
    };
}

static NSDictionary *MCMeasureBenchmarkWorkload(NSString *name, NSString *unit,
                                                 uint64_t workUnitsPerCommand,
                                                 MCBenchmarkCommandBlock makeCommand,
                                                 MCBenchmarkOutputValidator validateOutput) {
    const NSUInteger slotCount = 3;
    NSMutableArray<dispatch_semaphore_t> *slotSemaphores = [NSMutableArray arrayWithCapacity:slotCount];
    NSMutableArray<NSNumber *> *lastSerials = [NSMutableArray arrayWithCapacity:slotCount];
    for (NSUInteger slot = 0; slot < slotCount; slot++) {
        [slotSemaphores addObject:dispatch_semaphore_create(1)];
        [lastSerials addObject:@0];
    }

    id<MTLCommandBuffer> warmup = makeCommand(0, 0);
    MCRequire(warmup != nil, [NSString stringWithFormat:@"%@ warmup commandBuffer returned nil", name]);
    MCWaitForCommandBuffer(warmup);

    NSMutableArray<NSNumber *> *latencies = [NSMutableArray array];
    NSMutableArray<NSNumber *> *gpuTimes = [NSMutableArray array];
    NSMutableArray<NSString *> *errors = [NSMutableArray array];
    NSObject *completionLock = [[NSObject alloc] init];
    dispatch_group_t completionGroup = dispatch_group_create();
    const NSTimeInterval submissionSeconds = 2.0;
    uint64_t nextSerial = 1;
    NSUInteger submitted = 0;
    BOOL slotWaitTimedOut = NO;
    NSException *encodingException = nil;
    CFTimeInterval begin = CACurrentMediaTime();

    while (CACurrentMediaTime() - begin < submissionSeconds) {
        BOOL sawError = NO;
        @synchronized(completionLock) {
            sawError = errors.count > 0;
        }
        if (sawError) break;

        NSUInteger slot = (NSUInteger)((nextSerial - 1) % slotCount);
        dispatch_semaphore_t slotSemaphore = slotSemaphores[slot];
        dispatch_time_t slotDeadline = dispatch_time(DISPATCH_TIME_NOW,
                                                     (int64_t)MCCommandTimeoutSeconds * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(slotSemaphore, slotDeadline) != 0) {
            slotWaitTimedOut = YES;
            break;
        }

        @synchronized(completionLock) {
            sawError = errors.count > 0;
        }
        if (sawError) {
            dispatch_semaphore_signal(slotSemaphore);
            break;
        }

        BOOL groupEntered = NO;
        BOOL committed = NO;
        @try {
            id<MTLCommandBuffer> commandBuffer = makeCommand(slot, nextSerial);
            MCRequire(commandBuffer != nil, [NSString stringWithFormat:@"%@ commandBuffer returned nil", name]);
            lastSerials[slot] = @(nextSerial);
            CFTimeInterval submittedAt = CACurrentMediaTime();
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                CFTimeInterval completedAt = CACurrentMediaTime();
                @synchronized(completionLock) {
                    [latencies addObject:@((completedAt - submittedAt) * 1000.0)];
                    if (completed.GPUStartTime > 0.0 && completed.GPUEndTime > completed.GPUStartTime) {
                        [gpuTimes addObject:@((completed.GPUEndTime - completed.GPUStartTime) * 1000.0)];
                    }
                    if (completed.status != MTLCommandBufferStatusCompleted || completed.error != nil) {
                        NSString *message = completed.error.localizedDescription;
                        [errors addObject:message != nil ? message : @"command buffer did not complete successfully"];
                    }
                }
                dispatch_semaphore_signal(slotSemaphore);
                dispatch_group_leave(completionGroup);
            }];
            dispatch_group_enter(completionGroup);
            groupEntered = YES;
            [commandBuffer commit];
            committed = YES;
            submitted++;
            nextSerial++;
        } @catch (NSException *exception) {
            if (groupEntered && !committed) dispatch_group_leave(completionGroup);
            if (!committed) dispatch_semaphore_signal(slotSemaphore);
            encodingException = exception;
            break;
        }
    }

    dispatch_time_t drainDeadline = dispatch_time(DISPATCH_TIME_NOW,
                                                  (int64_t)MCCommandTimeoutSeconds * NSEC_PER_SEC);
    long drainResult = dispatch_group_wait(completionGroup, drainDeadline);
    CFTimeInterval end = CACurrentMediaTime();
    if (drainResult != 0) {
        MCAbort([NSString stringWithFormat:@"%@ command buffers did not drain within %lu seconds",
                 name, (unsigned long)MCCommandTimeoutSeconds]);
    }
    if (slotWaitTimedOut) {
        MCAbort([NSString stringWithFormat:@"%@ timed out waiting to reuse a command slot", name]);
    }
    if (encodingException != nil) @throw encodingException;

    NSArray<NSNumber *> *latencySnapshot = nil;
    NSArray<NSNumber *> *gpuTimeSnapshot = nil;
    NSArray<NSString *> *errorSnapshot = nil;
    @synchronized(completionLock) {
        latencySnapshot = [latencies copy];
        gpuTimeSnapshot = [gpuTimes copy];
        errorSnapshot = [errors copy];
    }
    MCRequire(errorSnapshot.count == 0,
              [NSString stringWithFormat:@"%@ command buffer error: %@", name, errorSnapshot.firstObject]);
    MCRequire(submitted > 0 && latencySnapshot.count == submitted,
              [NSString stringWithFormat:@"%@ did not complete all submitted command buffers", name]);
    MCRequire(validateOutput != nil, [NSString stringWithFormat:@"%@ has no output validator", name]);

    double wallSeconds = end - begin;
    MCRequire(wallSeconds > 0.0, [NSString stringWithFormat:@"%@ elapsed time was not positive", name]);
    NSArray<NSNumber *> *serialSnapshot = [lastSerials copy];
    validateOutput(serialSnapshot);

    uint64_t completedWork = (uint64_t)latencySnapshot.count * workUnitsPerCommand;
    double throughput = (double)completedWork / wallSeconds;
    NSMutableDictionary *result = [@{
        @"name": name,
        @"status": @"PASS",
        @"validated_output": @YES,
        @"last_serial_per_slot": serialSnapshot,
        @"submission_window_seconds": @(submissionSeconds),
        @"wall_seconds_including_drain": @(wallSeconds),
        @"submitted_command_buffers": @(submitted),
        @"completed_command_buffers": @(latencySnapshot.count),
        @"work_units": @(completedWork),
        @"score": @(throughput),
        @"throughput_per_second": @(throughput),
        @"throughput_unit": [unit stringByAppendingString:@"/s"],
        @"latency_ms": MCDistribution(latencySnapshot),
    } mutableCopy];
    if (gpuTimeSnapshot.count > 0) result[@"gpu_time_ms"] = MCDistribution(gpuTimeSnapshot);
    return result;
}

static simd_float4 MCBenchmarkColor(uint64_t serial) {
    float red = (float)((serial % 251u) + 1u) / 255.0f;
    float green = (float)(((serial / 251u) % 251u) + 1u) / 255.0f;
    return simd_make_float4(red, green, 0.5f, 1.0f);
}

static NSDictionary *MCRunBenchmarks(MCContext *context) {
    NSMutableArray<NSDictionary *> *workloads = [NSMutableArray array];
    NSString *failureReason = nil;
    BOOL aborted = NO;

    @try {
        const NSUInteger width = 512, height = 512;
        NSMutableArray<id<MTLTexture>> *targets = [NSMutableArray arrayWithCapacity:3];
        for (NSUInteger slot = 0; slot < 3; slot++) {
            [targets addObject:MCTexture(context, MTLPixelFormatRGBA8Unorm, width, height,
                                         MTLStorageModePrivate, MTLTextureUsageRenderTarget, NO)];
        }
        id<MTLRenderPipelineState> renderPipeline = MCSimpleRenderPipeline(context, @"mc_fullscreen_vertex",
                                                                            @"mc_benchmark_color_fragment",
                                                                            MTLPixelFormatRGBA8Unorm, 1);
        NSDictionary *render = MCMeasureBenchmarkWorkload(@"render-512x512", @"pixels", width * height,
                                                           ^id<MTLCommandBuffer>(NSUInteger slot, uint64_t serial) {
            id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
            MCRequire(commandBuffer != nil, @"render benchmark commandBuffer returned nil");
            MTLRenderPassDescriptor *pass = MCRenderPass(targets[slot], MTLLoadActionClear, MTLStoreActionStore);
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
            MCRequire(encoder != nil, @"render benchmark encoder returned nil");
            [encoder setRenderPipelineState:renderPipeline];
            simd_float4 color = MCBenchmarkColor(serial);
            [encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            return commandBuffer;
        }, ^(NSArray<NSNumber *> *lastSerials) {
            MCRequire(lastSerials.count == 3, @"render benchmark expected three slot tags");
            NSUInteger rowBytes = MCAlign(width * 4, 256);
            for (NSUInteger slot = 0; slot < 3; slot++) {
                uint64_t serial = lastSerials[slot].unsignedLongLongValue;
                MCRequire(serial > 0, [NSString stringWithFormat:@"render slot %lu was never rendered with a timed serial",
                                       (unsigned long)slot]);
                uint8_t expected[4] = {
                    (uint8_t)((serial % 251u) + 1u),
                    (uint8_t)(((serial / 251u) % 251u) + 1u),
                    128,
                    255,
                };
                NSData *image = MCReadPrivateTexture(context, targets[slot], 0, 0);
                const uint8_t *pixels = static_cast<const uint8_t *>(image.bytes);
                for (NSUInteger y = 0; y < height; y++) {
                    for (NSUInteger x = 0; x < width; x++) {
                        const uint8_t *pixel = pixels + y * rowBytes + x * 4;
                        for (NSUInteger channel = 0; channel < 4; channel++) {
                            int delta = (int)pixel[channel] - (int)expected[channel];
                            if (std::abs(delta) > 1) {
                                MCRequire(NO, [NSString stringWithFormat:@"render slot %lu serial %llu pixel (%lu,%lu) channel %lu expected %u±1 got %u",
                                               (unsigned long)slot, (unsigned long long)serial,
                                               (unsigned long)x, (unsigned long)y, (unsigned long)channel,
                                               (unsigned)expected[channel], (unsigned)pixel[channel]]);
                            }
                        }
                    }
                }
            }
        });
        [workloads addObject:render];
    } @catch (NSException *exception) {
        failureReason = exception.reason != nil ? exception.reason : exception.name;
        aborted = [exception.name isEqualToString:MCAbortName];
        [workloads addObject:@{@"name": @"render-512x512", @"status": @"FAIL", @"reason": failureReason}];
    }

    if (failureReason == nil) {
        @try {
            const uint32_t count = 1024u * 1024u;
            id<MTLBuffer> input = MCBuffer(context, (NSUInteger)count * sizeof(float), MTLResourceStorageModeShared);
            float *inputValues = static_cast<float *>(input.contents);
            for (uint32_t index = 0; index < count; index++) inputValues[index] = (float)(index & 1023u);
            NSMutableArray<id<MTLBuffer>> *outputs = [NSMutableArray arrayWithCapacity:3];
            for (NSUInteger slot = 0; slot < 3; slot++) {
                [outputs addObject:MCBuffer(context, (NSUInteger)count * sizeof(float), MTLResourceStorageModeShared)];
            }
            id<MTLComputePipelineState> pipeline = MCComputePipeline(context, @"mc_benchmark_compute");
            NSDictionary *compute = MCMeasureBenchmarkWorkload(@"compute-1M-float", @"float-elements", count,
                                                                ^id<MTLCommandBuffer>(NSUInteger slot, uint64_t serial) {
                MCRequire(serial <= 16000000u, @"compute serial exceeded exact f32 oracle range");
                uint32_t shaderSerial = (uint32_t)serial;
                id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
                MCRequire(commandBuffer != nil, @"compute benchmark commandBuffer returned nil");
                id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
                MCRequire(encoder != nil, @"compute benchmark encoder returned nil");
                [encoder setComputePipelineState:pipeline];
                [encoder setBuffer:input offset:0 atIndex:0];
                [encoder setBuffer:outputs[slot] offset:0 atIndex:1];
                [encoder setBytes:&count length:sizeof(count) atIndex:2];
                [encoder setBytes:&shaderSerial length:sizeof(shaderSerial) atIndex:3];
                [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                [encoder endEncoding];
                return commandBuffer;
            }, ^(NSArray<NSNumber *> *lastSerials) {
                MCRequire(lastSerials.count == 3, @"compute benchmark expected three slot tags");
                for (NSUInteger slot = 0; slot < 3; slot++) {
                    uint64_t serial = lastSerials[slot].unsignedLongLongValue;
                    MCRequire(serial > 0 && serial <= 16000000u,
                              [NSString stringWithFormat:@"compute slot %lu has invalid timed serial %llu",
                               (unsigned long)slot, (unsigned long long)serial]);
                    const float *output = static_cast<const float *>(outputs[slot].contents);
                    float serialValue = (float)serial;
                    for (uint32_t index = 0; index < count; index++) {
                        float expected = inputValues[index] * 3.0f + serialValue;
                        if (output[index] != expected) {
                            MCRequire(NO, [NSString stringWithFormat:@"compute slot %lu serial %llu index %u expected %.0f got %.0f",
                                           (unsigned long)slot, (unsigned long long)serial, index,
                                           (double)expected, (double)output[index]]);
                        }
                    }
                }
            });
            [workloads addObject:compute];
        } @catch (NSException *exception) {
            failureReason = exception.reason != nil ? exception.reason : exception.name;
            aborted = [exception.name isEqualToString:MCAbortName];
            [workloads addObject:@{@"name": @"compute-1M-float", @"status": @"FAIL", @"reason": failureReason}];
        }
    }

    if (failureReason == nil) {
        @try {
            const NSUInteger bytes = 16u * 1024u * 1024u;
            NSMutableArray<id<MTLBuffer>> *sources = [NSMutableArray arrayWithCapacity:3];
            NSMutableArray<id<MTLBuffer>> *destinations = [NSMutableArray arrayWithCapacity:3];
            for (NSUInteger slot = 0; slot < 3; slot++) {
                id<MTLBuffer> source = MCBuffer(context, bytes, MTLResourceStorageModeShared);
                std::memset(source.contents, 0x4d, bytes);
                [sources addObject:source];
                [destinations addObject:MCBuffer(context, bytes, MTLResourceStorageModePrivate)];
            }
            NSDictionary *blit = MCMeasureBenchmarkWorkload(@"blit-16MiB", @"bytes", bytes,
                                                             ^id<MTLCommandBuffer>(NSUInteger slot, uint64_t serial) {
                id<MTLBuffer> source = sources[slot];
                std::memcpy(source.contents, &serial, sizeof(serial));
                id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
                MCRequire(commandBuffer != nil, @"blit benchmark commandBuffer returned nil");
                id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
                MCRequire(encoder != nil, @"blit benchmark encoder returned nil");
                [encoder copyFromBuffer:source sourceOffset:0 toBuffer:destinations[slot] destinationOffset:0 size:bytes];
                [encoder endEncoding];
                return commandBuffer;
            }, ^(NSArray<NSNumber *> *lastSerials) {
                MCRequire(lastSerials.count == 3, @"blit benchmark expected three slot tags");
                id<MTLBuffer> readback = MCBuffer(context, bytes, MTLResourceStorageModeShared);
                for (NSUInteger slot = 0; slot < 3; slot++) {
                    uint64_t serial = lastSerials[slot].unsignedLongLongValue;
                    MCRequire(serial > 0, [NSString stringWithFormat:@"blit slot %lu was never copied with a timed serial",
                                           (unsigned long)slot]);
                    uint64_t sourceSerial = 0;
                    std::memcpy(&sourceSerial, sources[slot].contents, sizeof(sourceSerial));
                    MCRequire(sourceSerial == serial,
                              [NSString stringWithFormat:@"blit source slot %lu expected serial %llu got %llu",
                               (unsigned long)slot, (unsigned long long)serial, (unsigned long long)sourceSerial]);

                    id<MTLCommandBuffer> commandBuffer = [context.queue commandBuffer];
                    MCRequire(commandBuffer != nil, @"blit validation commandBuffer returned nil");
                    id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
                    MCRequire(encoder != nil, @"blit validation encoder returned nil");
                    [encoder copyFromBuffer:destinations[slot] sourceOffset:0 toBuffer:readback destinationOffset:0 size:bytes];
                    [encoder endEncoding];
                    MCWaitForCommandBuffer(commandBuffer);

                    const uint8_t *expected = static_cast<const uint8_t *>(sources[slot].contents);
                    const uint8_t *actual = static_cast<const uint8_t *>(readback.contents);
                    if (std::memcmp(actual, expected, bytes) != 0) {
                        NSUInteger offset = 0;
                        while (offset < bytes && actual[offset] == expected[offset]) offset++;
                        MCRequire(NO, [NSString stringWithFormat:@"blit slot %lu serial %llu mismatch at byte %lu: expected 0x%02x got 0x%02x",
                                       (unsigned long)slot, (unsigned long long)serial,
                                       (unsigned long)offset, (unsigned)expected[offset], (unsigned)actual[offset]]);
                    }
                }
            });
            [workloads addObject:blit];
        } @catch (NSException *exception) {
            failureReason = exception.reason != nil ? exception.reason : exception.name;
            aborted = [exception.name isEqualToString:MCAbortName];
            [workloads addObject:@{@"name": @"blit-16MiB", @"status": @"FAIL", @"reason": failureReason}];
        }
    }

    if (failureReason != nil) {
        for (NSString *name in @[@"render-512x512", @"compute-1M-float", @"blit-16MiB"]) {
            BOOL found = NO;
            for (NSDictionary *workload in workloads) {
                if ([workload[@"name"] isEqualToString:name]) { found = YES; break; }
            }
            if (!found) {
                [workloads addObject:@{@"name": name, @"status": @"NOT_RUN", @"reason": failureReason}];
            }
        }
        for (NSUInteger index = 0; index < workloads.count; index++) {
            NSDictionary *workload = workloads[index];
            if (workload[@"score"] != nil || workload[@"throughput_per_second"] != nil) {
                NSMutableDictionary *unscored = [workload mutableCopy];
                [unscored removeObjectForKey:@"score"];
                [unscored removeObjectForKey:@"throughput_per_second"];
                unscored[@"score_invalidated_reason"] = failureReason;
                [workloads replaceObjectAtIndex:index withObject:unscored];
            }
        }
    }
    BOOL passed = failureReason == nil && workloads.count == 3;
    NSMutableDictionary *result = [@{
        @"status": passed ? @"PASS" : @"FAIL",
        @"passed": @(passed),
        @"duration_seconds_per_workload": @2,
        @"max_in_flight_command_buffers": @3,
        @"api_selectors": @[@"commandBuffer", @"addCompletedHandler:", @"commit", @"dispatch_semaphore_wait",
                             @"dispatch_group_wait", @"GPUStartTime", @"GPUEndTime",
                             @"newRenderPipelineStateWithDescriptor:error:", @"newComputePipelineStateWithFunction:error:",
                             @"blitCommandEncoder", @"getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:"],
        @"workloads": workloads,
    } mutableCopy];
    if (failureReason != nil) result[@"reason"] = failureReason;
    if (aborted) result[@"aborted"] = @YES;
    return result;
}

static NSArray<NSString *> *MCCaseNames(void) {
    return @[
        @"01 shared buffer contents read/write",
        @"02 blit fill partial range and guards",
        @"03 shared-private-shared copy with offsets",
        @"04 affine compute dispatchThreads",
        @"05 affine compute dispatchThreadgroups",
        @"06 affine compute indirect dispatch",
        @"07 RGBA8 replaceRegion/getBytes padded rows",
        @"08 buffer-texture-buffer blit offsets/stride/guards",
        @"09 texture-to-texture region copy",
        @"10 private RGBA8 clear/readback",
        @"11 fullscreen triangle vertex_id and fragment",
        @"12 vertex descriptor indexed quad",
        @"13 instanced left/right quads",
        @"14 alpha blend red over blue",
        @"15 scissor rectangle",
        @"16 depth less ordering",
        @"17 stencil replace/equal mask",
        @"18 multiple render targets",
        @"19 4x MSAA resolve",
        @"20 nearest 2x2 texture sampling",
        @"21 uniform mipmap generation",
        @"22 texture view nonzero mip/slice",
        @"23 compute atomic add",
        @"24 threadgroup sum",
        @"25 function constant scales 2 and 3",
        @"26 MTLFence encoder dependency",
        @"27 MTLSharedEvent cross-queue dependency",
        @"28 IOSurface BGRA render/readback",
        @"29 IOSurface framebuffer rotation SPI",
        @"30 rows not 64-byte aligned: blit and texture over a buffer",
        @"31 CPU writes to a no-copy buffer reach the GPU",
        @"32 new pages at a reused address: no-copy buffers and IOSurfaces",
        @"33 small rectangles into a large texture (an atlas)",
        @"34 one texture rewritten and drawn by 16 command buffers in flight",
        @"35 IOSurface emptied (purgeable) and drawn again",
    ];
}

static NSArray<NSArray<NSString *> *> *MCCaseSelectors(void) {
    return @[
        @[@"newBufferWithLength:options:", @"contents"],
        @[@"newBufferWithLength:options:", @"commandBuffer", @"blitCommandEncoder", @"fillBuffer:range:value:", @"addCompletedHandler:", @"dispatch_semaphore_wait"],
        @[@"newBufferWithLength:options:", @"commandBuffer", @"blitCommandEncoder", @"copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"],
        @[@"newFunctionWithName:", @"newComputePipelineStateWithFunction:error:", @"setBuffer:offset:atIndex:", @"setBytes:length:atIndex:", @"dispatchThreads:threadsPerThreadgroup:"],
        @[@"newFunctionWithName:", @"newComputePipelineStateWithFunction:error:", @"dispatchThreadgroups:threadsPerThreadgroup:"],
        @[@"newBufferWithLength:options:", @"dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:"],
        @[@"newTextureWithDescriptor:", @"replaceRegion:mipmapLevel:withBytes:bytesPerRow:", @"getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:"],
        @[@"copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", @"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"],
        @[@"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:"],
        @[@"newTextureWithDescriptor:", @"renderCommandEncoderWithDescriptor:", @"loadAction", @"clearColor", @"storeAction"],
        @[@"newRenderPipelineStateWithDescriptor:error:", @"drawPrimitives:vertexStart:vertexCount:"],
        @[@"MTLVertexDescriptor", @"setVertexBuffer:offset:atIndex:", @"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"],
        @[@"MTLVertexDescriptor", @"instance_id", @"drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:"],
        @[@"blendingEnabled", @"sourceRGBBlendFactor", @"destinationRGBBlendFactor", @"drawPrimitives:vertexStart:vertexCount:"],
        @[@"setScissorRect:", @"drawPrimitives:vertexStart:vertexCount:"],
        @[@"depthAttachmentPixelFormat", @"newDepthStencilStateWithDescriptor:", @"setDepthStencilState:"],
        @[@"stencilAttachmentPixelFormat", @"frontFaceStencil", @"depthStencilPassOperation", @"newDepthStencilStateWithDescriptor:", @"setStencilReferenceValue:", @"setScissorRect:"],
        @[@"colorAttachments[1]", @"newRenderPipelineStateWithDescriptor:error:", @"drawPrimitives:vertexStart:vertexCount:"],
        @[@"supportsTextureSampleCount:", @"resolveTexture", @"MTLStoreActionMultisampleResolve"],
        @[@"newSamplerStateWithDescriptor:", @"setFragmentTexture:atIndex:", @"setFragmentSamplerState:atIndex:", @"texture.sample"],
        @[@"replaceRegion:mipmapLevel:withBytes:bytesPerRow:", @"generateMipmapsForTexture:", @"getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:"],
        @[@"replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:", @"newTextureViewWithPixelFormat:textureType:levels:slices:"],
        @[@"atomic_fetch_add_explicit", @"dispatchThreads:threadsPerThreadgroup:"],
        @[@"setThreadgroupMemoryLength:atIndex:", @"threadgroup_barrier", @"dispatchThreadgroups:threadsPerThreadgroup:"],
        @[@"MTLFunctionConstantValues", @"setConstantValue:type:atIndex:", @"newFunctionWithName:constantValues:error:"],
        @[@"newFence", @"updateFence:", @"waitForFence:"],
        @[@"newSharedEvent", @"encodeSignalEvent:value:", @"encodeWaitForEvent:value:", @"newCommandQueue"],
        @[@"IOSurfaceCreate", @"newTextureWithDescriptor:iosurface:plane:", @"IOSurfaceLock", @"IOSurfaceGetBaseAddress", @"IOSurfaceUnlock"],
        @[@"IOSurfaceCreate", @"newTextureWithDescriptor:iosurface:plane:", @"setRotation:", @"setFramebufferOnly:", @"setIsDrawable:",
          @"rotation", @"isFramebufferOnly", @"isDrawable", @"respondsToSelector:", @"setScissorRect:",
          @"setFragmentBytes:length:atIndex:", @"drawPrimitives:vertexStart:vertexCount:",
          @"IOSurfaceLock", @"IOSurfaceGetBaseAddress", @"IOSurfaceUnlock"],
        @[@"minimumLinearTextureAlignmentForPixelFormat:",
          @"copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
          @"newTextureWithDescriptor:offset:bytesPerRow:", @"setFragmentTexture:atIndex:", @"texture.sample"],
        @[@"newBufferWithBytesNoCopy:length:options:deallocator:", @"copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"],
        @[@"mmap MAP_FIXED", @"newBufferWithBytesNoCopy:length:options:deallocator:", @"IOSurfaceCreate",
          @"newTextureWithDescriptor:iosurface:plane:", @"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"],
        @[@"copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
          @"copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
          @"MTLLoadActionClear"],
        @[@"commit (16 in flight)", @"copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
          @"MTLLoadActionClear", @"setFragmentTexture:atIndex:", @"texture.sample"],
        @[@"IOSurfaceSetPurgeable", @"newTextureWithDescriptor:iosurface:plane:", @"IOSurfaceLock", @"IOSurfaceUnlock"],
    ];
}

static void MCRunCaseBody(MCContext *context, NSUInteger index) {
    switch (index) {
        case 0: MCTest01(context); break;
        case 1: MCTest02(context); break;
        case 2: MCTest03(context); break;
        case 3: MCTest04(context); break;
        case 4: MCTest05(context); break;
        case 5: MCTest06(context); break;
        case 6: MCTest07(context); break;
        case 7: MCTest08(context); break;
        case 8: MCTest09(context); break;
        case 9: MCTest10(context); break;
        case 10: MCTest11(context); break;
        case 11: MCTest12(context); break;
        case 12: MCTest13(context); break;
        case 13: MCTest14(context); break;
        case 14: MCTest15(context); break;
        case 15: MCTest16(context); break;
        case 16: MCTest17(context); break;
        case 17: MCTest18(context); break;
        case 18: MCTest19(context); break;
        case 19: MCTest20(context); break;
        case 20: MCTest21(context); break;
        case 21: MCTest22(context); break;
        case 22: MCTest23(context); break;
        case 23: MCTest24(context); break;
        case 24: MCTest25(context); break;
        case 25: MCTest26(context); break;
        case 26: MCTest27(context); break;
        case 27: MCTest28(context); break;
        case 28: MCTest29(context); break;
        case 29: MCTest30(context); break;
        case 30: MCTest31(context); break;
        case 31: MCTest32(context); break;
        case 32: MCTest33(context); break;
        case 33: MCTest34(context); break;
        case 34: MCTest35(context); break;
        default: MCRequire(NO, @"invalid fixed test index");
    }
}

static NSDictionary *MCNotRunCase(NSString *name, NSArray<NSString *> *selectors, NSString *reason) {
    return @{
        @"name": name,
        @"status": @"NOT_RUN",
        @"api_selectors": selectors,
        @"reason": reason,
        @"cpu_elapsed_ms": @0,
    };
}

static NSArray<NSString *> *MCSelectorsForCase(NSArray<NSString *> *selectors, NSUInteger index) {
    NSMutableOrderedSet<NSString *> *all = [NSMutableOrderedSet orderedSetWithArray:selectors];
    if (index != 0 && index != 6 && index != 21) {
        [all addObjectsFromArray:@[@"commandBuffer", @"addCompletedHandler:", @"commit",
                                   @"dispatch_semaphore_wait", @"endEncoding"]];
    }
    return all.array;
}

static NSDictionary *MCRunOneCase(MCContext *context, NSUInteger index,
                                   NSArray<NSString *> *nameList,
                                   NSArray<NSArray<NSString *> *> *selectorLists,
                                   BOOL *didAbort) {
    CFTimeInterval start = CACurrentMediaTime();
    NSString *status = @"PASS";
    NSString *reason = @"CPU oracle matched";
    BOOL aborted = NO;
    BOOL optional = index == 18;
    @try {
        MCRunCaseBody(context, index);
    } @catch (NSException *exception) {
        reason = exception.reason != nil ? exception.reason : exception.name;
        if ([exception.name isEqualToString:MCSkipName] && optional) {
            status = @"SKIP";
        } else {
            status = @"FAIL";
            if ([exception.name isEqualToString:MCSkipName]) {
                reason = [NSString stringWithFormat:@"unexpected SKIP for required test: %@", reason];
            }
        }
        aborted = [exception.name isEqualToString:MCAbortName];
    }
    CFTimeInterval elapsed = (CACurrentMediaTime() - start) * 1000.0;
    if (didAbort != nullptr) *didAbort = aborted;
    NSMutableDictionary *result = [@{
        @"name": nameList[index],
        @"status": status,
        @"api_selectors": MCSelectorsForCase(selectorLists[index], index),
        @"reason": reason,
        @"cpu_elapsed_ms": @(elapsed),
    } mutableCopy];
    if (aborted) result[@"aborted"] = @YES;
    return result;
}

static NSArray<NSString *> *MCCoveredSelectors(NSArray<NSDictionary *> *cases, NSDictionary *benchmark) {
    NSMutableSet<NSString *> *selectors = [NSMutableSet set];
    for (NSDictionary *testCase in cases) {
        if (![testCase[@"status"] isEqualToString:@"PASS"]) continue;
        [selectors addObjectsFromArray:@[@"MTLCreateSystemDefaultDevice", @"newLibraryWithFile:error:", @"newCommandQueue"]];
        for (NSString *selector in testCase[@"api_selectors"]) [selectors addObject:selector];
    }
    NSArray<NSString *> *benchmarkSelectors = benchmark[@"api_selectors"];
    if ([benchmark[@"status"] isEqualToString:@"PASS"] && benchmarkSelectors != nil) {
        for (NSString *selector in benchmarkSelectors) [selectors addObject:selector];
    }
    return [[selectors allObjects] sortedArrayUsingSelector:@selector(compare:)];
}

static NSDictionary *MCFixedDimensions(void) {
    return @{
        @"shared_buffer_bytes": @64,
        @"blit_fill_buffer_bytes": @128,
        @"offset_copy_bytes": @72,
        @"affine_compute_elements": @1003,
        @"indirect_compute_elements": @1024,
        @"texture_replace_region": @{@"width": @7, @"height": @5, @"row_bytes": @256},
        @"texture_blit_region": @{@"width": @7, @"height": @4, @"row_bytes": @256},
        @"texture_copy": @{@"width": @8, @"height": @6, @"region_width": @3, @"region_height": @2},
        @"render_targets": @{@"width": @32, @"height": @32, @"format": @"RGBA8Unorm"},
        @"render_vertex_quad": @{@"width": @32, @"height": @32, @"format": @"RGBA8Unorm"},
        @"msaa_sample_count": @4,
        @"nearest_sample": @{@"source_width": @2, @"source_height": @2, @"destination_width": @32, @"destination_height": @32},
        @"mipmap_base": @{@"width": @16, @"height": @16},
        @"texture_view": @{@"array_width": @8, @"array_height": @8, @"array_length": @2,
                            @"mip_level": @1, @"view_width": @4, @"view_height": @4, @"slice": @1},
        @"atomic_add_count": @1024,
        @"threadgroup_sum_elements": @16,
        @"function_constant_elements": @257,
        @"fence_dependency_elements": @257,
        @"shared_event_elements": @257,
        @"iosurface": @{@"width": @32, @"height": @32, @"format": @"BGRA8Unorm", @"bytes_per_row": @256},
        @"iosurface_framebuffer_rotation_spi": @{@"physical_width": @256, @"physical_height": @64,
                                                   @"format": @"BGRA8Unorm", @"bytes_per_element": @4,
                                                   @"bytes_per_row": @1024,
                                                   @"logical_texture_dimensions": @{
                                                       @"rotations_1_3_4": @{@"width": @64, @"height": @256},
                                                       @"rotation_2": @{@"width": @256, @"height": @64},
                                                   },
                                                   @"rotations": @[@1, @2, @3, @4]},
        @"unaligned_rows": @{@"widths": @[@7, @13, @61, @77, @100, @333], @"height": @9, @"format": @"RGBA8Unorm",
                              @"bytes_per_row": @"width * 4 rounded to the linear alignment, never a multiple of 64"},
        @"no_copy_coherency": @{@"bytes": @262144, @"rounds": @8, @"line_bytes": @64},
        @"reused_address": @{@"bytes": @65536, @"rounds": @6, @"iosurface": @{@"width": @128, @"height": @128, @"format": @"BGRA"}},
        @"atlas": @{@"width": @1024, @"height": @512, @"format": @"BGRA8Unorm", @"rectangles_from_buffers": @8,
                    @"rectangles_from_textures": @3},
        @"in_flight_hazards": @{@"width": @256, @"height": @64, @"command_buffers": @16, @"writes": @[@"blit", @"render pass clear"]},
        @"purged_surface": @{@"width": @256, @"height": @256, @"format": @"BGRA"},
        @"benchmark_render": @{@"width": @512, @"height": @512, @"seconds": @2},
        @"benchmark_compute_floats": @{@"count": @1048576, @"seconds": @2},
        @"benchmark_blit_bytes": @{@"bytes": @16777216, @"seconds": @2},
    };
}

static NSArray<NSString *> *MCUncoveredCategories(void) {
    return @[
        @"argument buffers",
        @"indirect command buffers (ICB)",
        @"sparse textures",
        @"ray tracing",
        @"mesh shaders",
        @"tessellation",
        @"function pointers and dynamic linking",
        @"heaps and aliasing",
        @"counter sampling and performance counters",
        @"complete Metal conformance coverage",
    ];
}

static NSDictionary *MCBaseReport(id<MTLDevice> device, NSString *libraryPath) {
    NSString *name = device.name != nil ? device.name : @"unknown Metal device";
    return @{
        @"passed": @NO,
        @"scope": @"fixed deterministic correctness probes, including a private MTLTextureDescriptor framebuffer rotation SPI probe; not a complete Metal conformance suite",
        @"case_elapsed_note": @"Host elapsed time for each case, including bounded GPU completion waits; not process CPU time.",
        @"device_name": name,
        @"os_version": NSProcessInfo.processInfo.operatingSystemVersionString,
        @"metallib_path": libraryPath != nil ? libraryPath : @"",
        @"setup_api_selectors": @[@"MTLCreateSystemDefaultDevice", @"newLibraryWithFile:error:", @"newCommandQueue"],
        @"msl_version": @"2.3",
        @"fixed_test_dimensions": MCFixedDimensions(),
        @"covered_selectors": @[],
        @"uncovered_categories": MCUncoveredCategories(),
        @"correctness_cases": @[],
        @"correctness_case_count": @35,
        @"benchmark": @{@"status": @"NOT_REQUESTED", @"passed": [NSNull null], @"workloads": @[]},
    };
}

NSDictionary *MCRun(NSString *libraryPath, bool benchmark, void (^progress)(NSString *)) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSMutableDictionary *report = [MCBaseReport(device, libraryPath) mutableCopy];
        NSArray<NSString *> *names = MCCaseNames();
        NSArray<NSArray<NSString *> *> *selectors = MCCaseSelectors();
        id<MTLLibrary> library = nil;
        id<MTLCommandQueue> queue = nil;
        NSString *setupError = nil;

        if (device == nil) {
            setupError = @"MTLCreateSystemDefaultDevice returned nil";
        } else if (libraryPath.length == 0) {
            setupError = @"offline metallib path is empty";
        } else {
            NSError *libraryError = nil;
            library = [device newLibraryWithFile:libraryPath error:&libraryError];
            if (library == nil) {
                setupError = [NSString stringWithFormat:@"newLibraryWithFile:error: failed: %@",
                              libraryError.localizedDescription != nil ? libraryError.localizedDescription : @"no NSError"];
            }
        }
        if (setupError == nil) {
            queue = [device newCommandQueue];
            if (queue == nil) setupError = @"newCommandQueue returned nil";
        }

        NSMutableArray<NSDictionary *> *caseResults = [NSMutableArray arrayWithCapacity:names.count];
        BOOL aborted = NO;
        MCContext *context = nil;
        if (setupError != nil) {
            for (NSUInteger index = 0; index < names.count; index++) {
                [caseResults addObject:MCNotRunCase(names[index], MCSelectorsForCase(selectors[index], index), setupError)];
            }
        } else {
            context = [[MCContext alloc] init];
            context.device = device;
            context.queue = queue;
            context.library = library;
            for (NSUInteger index = 0; index < names.count; index++) {
                if (progress != nil) progress([NSString stringWithFormat:@"Running %@", names[index]]);
                BOOL caseAborted = NO;
                NSDictionary *result = MCRunOneCase(context, index, names, selectors, &caseAborted);
                [caseResults addObject:result];
                if (progress != nil) {
                    progress([NSString stringWithFormat:@"%@ %@: %@ (%.2f ms)",
                              [result[@"status"] isEqualToString:@"PASS"] ? @"PASS" : result[@"status"],
                              names[index], result[@"reason"], [result[@"cpu_elapsed_ms"] doubleValue]]);
                }
                if (caseAborted) {
                    aborted = YES;
                    for (NSUInteger remaining = index + 1; remaining < names.count; remaining++) {
                        [caseResults addObject:MCNotRunCase(names[remaining], MCSelectorsForCase(selectors[remaining], remaining),
                                                            [NSString stringWithFormat:@"not run after %@ timed out", names[index]])];
                    }
                    break;
                }
            }
        }

        BOOL correctnessPassed = setupError == nil && !aborted;
        NSUInteger passCount = 0, skipCount = 0, failCount = 0, notRunCount = 0;
        for (NSUInteger index = 0; index < caseResults.count; index++) {
            NSString *status = caseResults[index][@"status"];
            if ([status isEqualToString:@"PASS"]) passCount++;
            else if ([status isEqualToString:@"SKIP"] && index == 18) skipCount++;
            else if ([status isEqualToString:@"NOT_RUN"]) notRunCount++;
            else failCount++;
        }
        correctnessPassed = correctnessPassed && failCount == 0 && notRunCount == 0 &&
            (passCount + skipCount == names.count);

        NSDictionary *benchmarkResult;
        if (!benchmark) {
            benchmarkResult = @{@"status": @"NOT_REQUESTED", @"passed": [NSNull null], @"workloads": @[]};
        } else if (!correctnessPassed) {
            benchmarkResult = @{
                @"status": @"NOT_RUN",
                @"passed": @NO,
                @"reason": @"benchmarks require all required correctness cases to PASS",
                @"workloads": @[],
            };
        } else {
            if (progress != nil) progress(@"Running three timed Metal workloads (2 seconds each)");
            @try {
                benchmarkResult = MCRunBenchmarks(context);
            } @catch (NSException *exception) {
                NSString *reason = exception.reason != nil ? exception.reason : exception.name;
                benchmarkResult = @{
                    @"status": @"FAIL",
                    @"passed": @NO,
                    @"reason": reason,
                    @"aborted": @([exception.name isEqualToString:MCAbortName]),
                    @"workloads": @[],
                };
            }
        }

        BOOL totalPassed = correctnessPassed && (!benchmark || [benchmarkResult[@"passed"] boolValue]);
        report[@"passed"] = @(totalPassed);
        report[@"correctness_passed"] = @(correctnessPassed);
        report[@"correctness_summary"] = @{
            @"pass": @(passCount), @"skip": @(skipCount), @"fail": @(failCount), @"not_run": @(notRunCount),
        };
        report[@"correctness_cases"] = caseResults;
        report[@"covered_selectors"] = MCCoveredSelectors(caseResults, benchmarkResult);
        report[@"benchmark"] = benchmarkResult;
        if (setupError != nil) report[@"setup_error"] = setupError;
        if (aborted) report[@"aborted"] = @YES;
        if (progress != nil) {
            progress([NSString stringWithFormat:@"Metal Check %@", totalPassed ? @"PASS" : @"FAIL"]);
        }
        return report;
    }
}

static NSDictionary *MCProcessMemory(void) {
    task_vm_info_data_t info = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return @{};
    return @{@"resident_bytes": @(info.resident_size), @"physical_footprint_bytes": @(info.phys_footprint)};
}

NSDictionary *MCRunStress(NSString *libraryPath, double seconds, NSUInteger memoryMiB,
                          bool (^shouldStop)(void), void (^progress)(NSString *)) {
    @autoreleasepool {
        if (!std::isfinite(seconds) || seconds < 1 || seconds > 86400 || memoryMiB > 512) {
            return @{@"passed": @NO, @"error": @"Stress duration must be 1–86400 seconds; extra memory must be 0–512 MiB"};
        }
        NSMutableDictionary *report = [MCRun(libraryPath, false, progress) mutableCopy];
        report[@"mode"] = @"stress";
        if (![report[@"passed"] boolValue]) {
            report[@"stress"] = @{@"status": @"NOT_RUN", @"reason": @"All required correctness cases must pass before stress"};
            return report;
        }
        NSMutableArray<NSDictionary *> *rounds = [NSMutableArray array];
        CFTimeInterval started = CACurrentMediaTime();
        NSString *failure = nil;
        BOOL cancelled = NO;
        BOOL aborted = NO;
        @try {
            MCContext *context = [[MCContext alloc] init];
            context.device = MTLCreateSystemDefaultDevice();
            MCRequire(context.device != nil, @"stress device creation returned nil");
            context.queue = [context.device newCommandQueue];
            NSError *libraryError = nil;
            context.library = [context.device newLibraryWithFile:libraryPath error:&libraryError];
            MCRequire(context.queue != nil && context.library != nil, @"stress queue/library setup failed");
            NSMutableArray<id<MTLBuffer>> *resident = [NSMutableArray array];
            NSUInteger remaining = memoryMiB * 1024u * 1024u;
            while (remaining > 0) {
                NSUInteger bytes = MIN(remaining, 8u * 1024u * 1024u);
                [resident addObject:MCBuffer(context, bytes, MTLResourceStorageModeShared)];
                remaining -= bytes;
            }
            // Each round keeps these allocations live while recreating render/compute/blit
            // resources. GPU writes touch all extra pages; CPU checks every byte after load.
            while (CACurrentMediaTime() - started < seconds || rounds.count == 0) {
                @autoreleasepool {
                    if (shouldStop != nil && shouldStop()) { cancelled = YES; break; }
                    NSUInteger round = rounds.count + 1;
                    if (progress != nil) progress([NSString stringWithFormat:@"Stress round %lu, %.0f / %.0f s, %lu MiB extra resident buffers",
                                                   (unsigned long)round, CACurrentMediaTime() - started, seconds, (unsigned long)memoryMiB]);
                    if (resident.count > 0) {
                        id<MTLCommandBuffer> cb = [context.queue commandBuffer];
                        MCRequire(cb != nil, @"stress fill command buffer returned nil");
                        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                        MCRequire(blit != nil, @"stress fill encoder returned nil");
                        for (NSUInteger slot = 0; slot < resident.count; slot++) {
                            uint8_t tag = (uint8_t)(1 + ((round + slot) % 251));
                            [blit fillBuffer:resident[slot] range:NSMakeRange(0, resident[slot].length) value:tag];
                        }
                        [blit endEncoding];
                        MCWaitForCommandBuffer(cb);
                    }
                    NSDictionary *work = MCRunBenchmarks(context);
                    if (![work[@"passed"] boolValue]) {
                        [rounds addObject:@{@"round": @(round), @"benchmark": work, @"memory": MCProcessMemory()}];
                        if ([work[@"aborted"] boolValue]) MCAbort(work[@"reason"] ?: @"stress workload timed out");
                        MCRequire(NO, work[@"reason"] ?: @"stress workload failed");
                    }
                    for (NSUInteger slot = 0; slot < resident.count; slot++) {
                        id<MTLBuffer> buffer = resident[slot];
                        const uint8_t *bytes = static_cast<const uint8_t *>(buffer.contents);
                        const NSUInteger length = buffer.length;
                        uint8_t expected = (uint8_t)(1 + ((round + slot) % 251));
                        for (NSUInteger offset = 0; offset < length; offset++) {
                            if (bytes[offset] != expected) MCRequire(NO, [NSString stringWithFormat:@"stress resident slot %lu byte %lu: expected %u got %u",
                                (unsigned long)slot, (unsigned long)offset, (unsigned)expected, (unsigned)bytes[offset]]);
                        }
                    }
                    [rounds addObject:@{@"round": @(round), @"benchmark": work, @"memory": MCProcessMemory(),
                                        @"elapsed_seconds": @(CACurrentMediaTime() - started), @"resident_output_validated": @YES}];
                    if (progress != nil) progress([NSString stringWithFormat:@"PASS stress round %lu (render, compute, blit, resident memory)", (unsigned long)round]);
                }
            }
            if (shouldStop != nil && shouldStop()) cancelled = YES;
        } @catch (NSException *exception) {
            failure = exception.reason ?: exception.name;
            aborted = [exception.name isEqualToString:MCAbortName];
        }
        NSUInteger completedRounds = 0;
        for (NSDictionary *round in rounds) if ([round[@"resident_output_validated"] boolValue]) completedRounds++;
        BOOL passed = failure == nil && !cancelled && completedRounds > 0;
        NSMutableDictionary *stress = [@{@"status": failure != nil ? @"FAIL" : cancelled ? @"CANCELLED" : @"PASS",
            @"requested_seconds": @(seconds), @"elapsed_seconds": @(CACurrentMediaTime() - started),
            @"extra_resident_mib": @(memoryMiB), @"rounds": rounds, @"completed_rounds": @(completedRounds),
            @"cancelled": @(cancelled), @"aborted": @(aborted),
            @"score_note": @"Per-round workload rates under stress, not an isolated performance baseline; checks and allocation churn are outside timed windows."
        } mutableCopy];
        if (failure != nil) stress[@"reason"] = failure;
        report[@"stress"] = stress;
        report[@"passed"] = @(passed);
        if (progress != nil) progress([NSString stringWithFormat:@"Metal stress %@", stress[@"status"]]);
        return report;
    }
}
