// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include "../NativeBridgeMetalFrontend.h"

static bool pixels(id<MTLTexture> texture) {
    uint8_t data[16 * 16 * 4] = {};
    [texture getBytes:data bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
    const uint8_t expected[4] = {0, 255, 0, 255};
    for (unsigned i = 0; i < 256; ++i)
        if (memcmp(data + i * 4, expected, 4)) return false;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!device) return 2;
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = 4;
        descriptor.storageMode = MTLStorageModeMemoryless;
        id<MTLTexture> source = [device newTextureWithDescriptor:descriptor];
        descriptor.textureType = MTLTextureType2D;
        descriptor.sampleCount = 1;
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
        if (!source || !target || source.sampleCount != 1 || source.textureType != MTLTextureType2D) return 1;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        for (unsigned mode = 0; mode < 3; ++mode) {
            const uint8_t black[16 * 16 * 4] = {};
            [target replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:black bytesPerRow:64];
            auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
            auto attachment = pass.colorAttachments[0];
            attachment.texture = source;
            attachment.resolveTexture = target;
            attachment.loadAction = mode == 2 ? MTLLoadActionLoad : MTLLoadActionClear;
            attachment.storeAction = mode == 1 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
            attachment.clearColor = MTLClearColorMake(0, 1, 0, 1);
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return 1;
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted || !pixels(target)) {
                std::fprintf(stderr, "FAIL resolve mode %u: %s\n", mode, commands.error.localizedDescription.UTF8String ?: "pixels");
                return 1;
            }
        }
        std::puts("PASS: single-sample discard resolve, preserved source and load resolve produce GPU pixels");
    }
}
