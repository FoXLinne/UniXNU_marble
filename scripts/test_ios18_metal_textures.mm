// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include "NativeBridgeMetalFrontend.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv) {
    @autoreleasepool {
        if(argc!=3)return 2;
        id<MTLDevice> device=native_bridge_metal_open(argv[1],argv[2],0x6d617262,nullptr);
        CHECK(device);
        unsigned checked=0;
        for(NSUInteger samples: {2u,4u}) {
            if(![device supportsTextureSampleCount:samples])continue;
            MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
            d.textureType=MTLTextureType2DMultisample;d.sampleCount=samples;
            d.storageMode=MTLStorageModeMemoryless;d.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> source=[device newTextureWithDescriptor:d];
            CHECK(source && source.sampleCount==samples && source.textureType==MTLTextureType2DMultisample);
            CHECK(source.storageMode==MTLStorageModeMemoryless);
            d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
            d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
            id<MTLTexture> target=[device newTextureWithDescriptor:d];CHECK(target);
            id<MTLCommandBuffer> cb=[[device newCommandQueue] commandBuffer];
            MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=source;pass.colorAttachments[0].resolveTexture=target;
            pass.colorAttachments[0].loadAction=MTLLoadActionClear;
            pass.colorAttachments[0].storeAction=MTLStoreActionMultisampleResolve;
            pass.colorAttachments[0].clearColor=MTLClearColorMake(0.25,0.5,0.75,1);
            id<MTLRenderCommandEncoder> encoder=[cb renderCommandEncoderWithDescriptor:pass];CHECK(encoder);
            [encoder endEncoding];[cb commit];[cb waitUntilCompleted];
            if(cb.status!=MTLCommandBufferStatusCompleted)std::fprintf(stderr,"%s\n",cb.error.description.UTF8String);
            CHECK(cb.status==MTLCommandBufferStatusCompleted);
            unsigned char pixels[8*8*4]={};
            [target getBytes:pixels bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
            for(unsigned i=0;i<64;i++) {
                CHECK(std::abs((int)pixels[i*4]-191)<=1);
                CHECK(std::abs((int)pixels[i*4+1]-128)<=1);
                CHECK(std::abs((int)pixels[i*4+2]-64)<=1);
                CHECK(pixels[i*4+3]==255);
            }
            ++checked;
        }
        CHECK(checked);
        NSDictionary *planes=@{(id)kIOSurfaceWidth:@64,(id)kIOSurfaceHeight:@64,(id)kIOSurfacePixelFormat:@((uint32_t)'420f'),
            (id)kIOSurfaceAllocSize:@16384,
            (id)kIOSurfacePlaneInfo:@[
                @{(id)kIOSurfacePlaneWidth:@64,(id)kIOSurfacePlaneHeight:@64,(id)kIOSurfacePlaneBytesPerRow:@64,
                  (id)kIOSurfacePlaneBytesPerElement:@1,(id)kIOSurfacePlaneOffset:@0,(id)kIOSurfacePlaneSize:@4096},
                @{(id)kIOSurfacePlaneWidth:@32,(id)kIOSurfacePlaneHeight:@32,(id)kIOSurfacePlaneBytesPerRow:@64,
                  (id)kIOSurfacePlaneBytesPerElement:@2,(id)kIOSurfacePlaneOffset:@4096,(id)kIOSurfacePlaneSize:@2048}]};
        IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)planes);CHECK(surface && IOSurfaceGetPlaneCount(surface)==2);
        const int baselineUseCount=IOSurfaceGetUseCount(surface);
        for(NSUInteger plane=0;plane<2;plane++) {
          @autoreleasepool {
            MTLPixelFormat format=plane?MTLPixelFormatRG8Unorm:MTLPixelFormatR8Unorm;
            MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:plane?32:64 height:plane?32:64 mipmapped:NO];
            d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
            id<MTLTexture> texture=[device newTextureWithDescriptor:d iosurface:surface plane:plane];
            CHECK(texture && texture.iosurface==surface && texture.iosurfacePlane==plane);
            id<MTLTexture> view=[texture newTextureViewWithPixelFormat:format];
            CHECK(view && view.iosurface==surface && view.iosurfacePlane==plane);
            CHECK(IOSurfaceGetUseCount(surface)>=baselineUseCount+2);
          }
        }
        CHECK(IOSurfaceGetUseCount(surface)==baselineUseCount);
        CFRelease(surface);
        std::printf("PASS: %u MSAA memoryless resolve pixel cases and two IOSurface planes with views\n",checked);
    }
}
