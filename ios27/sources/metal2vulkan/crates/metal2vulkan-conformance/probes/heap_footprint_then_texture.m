// Plain Metal's own answer, with the API validation layer OFF, to both halves of one question for
// a format this crate holds no Vulkan image of: does
// `heapTextureSizeAndAlignWithDescriptor:` answer a footprint, and does
// `newTextureWithDescriptor:` -- on the device, on an automatic heap and at an offset of a
// placement heap -- then answer a texture or nil?
//
// The pair is the specification for the runtime's two paths. Answering a footprint and refusing
// the texture is only right if Metal does both.
//
// Build (no GPU needed to build):
//
//   clang -fobjc-arc -fmodules -framework Metal -framework Foundation \
//       -o heap_footprint_then_texture heap_footprint_then_texture.m
//
// Run one shape per process, WITHOUT MTL_DEBUG_LAYER (the layer is not passive: it asserts, and
// it has been measured to change what a shader reads):
//
//   ./heap_footprint_then_texture <shape>
//   ./heap_footprint_then_texture --list
//
// Every shape prints `DEVICE <name> / RELEASE <os>`, then `SHAPE <shape>`, then `READ <key>
// <value>` lines, then `DONE <shape>`. A shape whose process Metal ends prints no `DONE`, and its
// exit status is the reading.
//
// A shape names one pixel format by its raw `MTLPixelFormat` value, so a private format Metal
// describes but no header names is as reachable as a public one. Each asks a 16 by 16 2D
// `ShaderRead` descriptor in `Shared` storage -- the descriptor the runtime's heap-texture sweep
// asks -- and then the same descriptor in `Private`.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static id<MTLDevice> gDevice = nil;

static const NSUInteger kHeapBytes = 4u << 20;

static void head(void) {
    printf("DEVICE %s\n", gDevice.name.UTF8String);
    printf("RELEASE macOS %s\n",
           [NSProcessInfo processInfo].operatingSystemVersionString.UTF8String);
    fflush(stdout);
}

static void readv(const char *key, const char *fmt, ...) {
    printf("READ %s ", key);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/// The 16 by 16 2D `ShaderRead` descriptor of `format` in `mode`.
static MTLTextureDescriptor *descriptorOf(NSUInteger format, MTLStorageMode mode) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2D;
    d.pixelFormat = (MTLPixelFormat)format;
    d.width = 16;
    d.height = 16;
    d.depth = 1;
    d.mipmapLevelCount = 1;
    d.sampleCount = 1;
    d.arrayLength = 1;
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = mode;
    return d;
}

/// Both halves for one storage mode: the heap footprint, then the three creators.
static void bothHalves(NSUInteger format, MTLStorageMode mode, const char *word) {
    MTLTextureDescriptor *d = descriptorOf(format, mode);
    readv("format-name", "%s %s", word, d.description.UTF8String ? "described" : "undescribed");
    MTLSizeAndAlign sa = [gDevice heapTextureSizeAndAlignWithDescriptor:d];
    readv("size-and-align", "%s size=%lu align=%lu", word, (unsigned long)sa.size,
          (unsigned long)sa.align);

    id<MTLTexture> plain = [gDevice newTextureWithDescriptor:d];
    readv("device-texture", "%s %s", word, plain ? "texture" : "nil");
    if (plain) {
        readv("device-texture-shape", "%s format=%lu width=%lu height=%lu allocated=%lu", word,
              (unsigned long)plain.pixelFormat, (unsigned long)plain.width,
              (unsigned long)plain.height, (unsigned long)plain.allocatedSize);
    }

    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = kHeapBytes;
    hd.storageMode = mode;
    hd.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    hd.hazardTrackingMode = MTLHazardTrackingModeTracked;

    hd.type = MTLHeapTypeAutomatic;
    id<MTLHeap> automatic = [gDevice newHeapWithDescriptor:hd];
    readv("automatic-heap", "%s %s", word, automatic ? "heap" : "nil");
    if (automatic) {
        id<MTLTexture> t = [automatic newTextureWithDescriptor:d];
        readv("automatic-heap-texture", "%s %s", word, t ? "texture" : "nil");
        if (t) {
            readv("automatic-heap-texture-shape", "%s allocated=%lu heapOffset=%lu", word,
                  (unsigned long)t.allocatedSize, (unsigned long)t.heapOffset);
        }
    }

    hd.type = MTLHeapTypePlacement;
    id<MTLHeap> placement = [gDevice newHeapWithDescriptor:hd];
    readv("placement-heap", "%s %s", word, placement ? "heap" : "nil");
    if (placement) {
        id<MTLTexture> t = [placement newTextureWithDescriptor:d offset:0];
        readv("placement-heap-texture", "%s %s", word, t ? "texture" : "nil");
        if (t) {
            readv("placement-heap-texture-shape", "%s allocated=%lu heapOffset=%lu", word,
                  (unsigned long)t.allocatedSize, (unsigned long)t.heapOffset);
        }
    }
}

/// One shape: both storage modes of one raw pixel-format value.
static void shapeOf(NSUInteger format) {
    bothHalves(format, MTLStorageModeShared, "shared");
    bothHalves(format, MTLStorageModePrivate, "private");
}

struct shape {
    const char *name;
    NSUInteger format;
};

// The six ASTC footprints with no Vulkan block, the two plain ASTC formats of a Vulkan block
// this crate does hold (a control: the same arithmetic where the image exists), one of each
// other family the footprint table answers for, a format whose descriptor Metal's own validation
// stops, and the two packed 10-bit formats Metal describes and makes no texture of.
static const struct shape kShapes[] = {
    {"astc-4x2-srgb", 185},
    {"astc-4x2-ldr", 203},
    {"astc-4x2-hdr", 221},
    {"astc-8x4-srgb", 191},
    {"astc-8x4-ldr", 209},
    {"astc-8x4-hdr", 227},
    {"astc-4x4-srgb-held", 186},
    {"astc-4x4-ldr-held", 204},
    {"rgba8unorm-held", 70},
    {"argb8unorm-srgb", 85},
    {"rgb8unorm", 45},
    {"r10unorm-packed", 588},
    {"rg10unorm-packed", 589},
};

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--list") == 0) {
            for (size_t i = 0; i < sizeof kShapes / sizeof *kShapes; i++) {
                printf("%s\n", kShapes[i].name);
            }
            return 0;
        }
        if (argc != 2) {
            fprintf(stderr, "usage: %s <shape>|--list\n", argv[0]);
            return 2;
        }
        gDevice = MTLCreateSystemDefaultDevice();
        if (!gDevice) {
            fprintf(stderr, "no Metal device\n");
            return 2;
        }
        head();
        for (size_t i = 0; i < sizeof kShapes / sizeof *kShapes; i++) {
            if (strcmp(argv[1], kShapes[i].name) != 0) {
                continue;
            }
            printf("SHAPE %s\n", kShapes[i].name);
            fflush(stdout);
            shapeOf(kShapes[i].format);
            printf("DONE %s\n", kShapes[i].name);
            fflush(stdout);
            return 0;
        }
        fprintf(stderr, "unknown shape %s\n", argv[1]);
        return 2;
    }
}
