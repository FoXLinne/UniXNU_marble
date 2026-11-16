// Metal's private texture fills over a texture made on a buffer: which bytes one fill changed.
//
// Re-measures the four buffer rows of the conformance case `surface-fills-over-planes-and-buffers`
// (formats 576, 577, 578 and 579 over an 8x4 or 7x3 texture on a shared buffer, every byte 0xEE
// first), printing every byte of every changed run. Three of the four are already carried, so a
// run of this probe that reproduces them says its fourth row is trustworthy; the fourth, a color
// packed in RGBA8Unorm over the whole of a 579 texture, has 32-byte runs that the runtime lane's
// formatter truncated at 24.
//
// Calls the Apple SPI fillTexture:level:slice:region:bytes:length: and
// fillTexture:level:slice:region:color:pixelFormat:, whose selectors are checked before use.
//
// clang -fobjc-arc -framework Metal -framework Foundation -o surface_buffer_fills \
//     surface_buffer_fills.m

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

@protocol M2VPrivateFills <MTLBlitCommandEncoder>
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              bytes:(const void *)bytes
             length:(NSUInteger)length;
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color
        pixelFormat:(MTLPixelFormat)pixelFormat;
@end

// The byte every buffer starts as, so a fill's bytes are the ones that are no longer it.
static const uint8_t kBefore = 0xEE;

// The runs of bytes other than 0xEE, as ` [<at>+<n>: <bytes>]`, every byte printed.
static void printRuns(const uint8_t *bytes, NSUInteger length) {
    NSUInteger i = 0;
    while (i < length) {
        if (bytes[i] == kBefore) { i += 1; continue; }
        NSUInteger j = i;
        while (j < length && bytes[j] != kBefore) { j += 1; }
        if (i == 0) { printf(" [0+%lu:", (unsigned long)(j - i)); }
        else { printf(" [0x%lx+%lu:", (unsigned long)i, (unsigned long)(j - i)); }
        for (NSUInteger at = i; at < j; at += 1) { printf(" %02x", bytes[at]); }
        printf("]");
        i = j;
    }
}

// One row: the pixel format, the texture's size, the buffer's bytesPerRow, and the fill.
typedef struct {
    MTLPixelFormat format;
    NSUInteger width;
    NSUInteger height;
    NSUInteger bytesPerRow;
    const char *fill;            // the lane's case word
    MTLRegion region;
    NSUInteger patternBytes;     // 0 for a color fill
    MTLPixelFormat packedInto;   // the color fill's format
} Row;

int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) { fprintf(stderr, "no Metal device\n"); return 2; }
        printf("DEVICE %s\n", device.name.UTF8String);

        SEL patternSel = @selector(fillTexture:level:slice:region:bytes:length:);
        SEL colorSel = @selector(fillTexture:level:slice:region:color:pixelFormat:);

        id<MTLCommandQueue> queue = [device newCommandQueue];

        const Row rows[] = {
            {576, 8, 4, 64, "x5,2,3", {{5, 2, 0}, {3, 2, 1}}, 3, 0},
            {577, 7, 3, 32, "b4", {{0, 0, 0}, {7, 3, 1}}, 4, 0},
            {578, 8, 4, 64, "r3", {{1, 1, 0}, {2, 1, 1}}, 3, 0},
            {579, 8, 4, 64, "p70", {{0, 0, 0}, {8, 4, 1}}, 0, MTLPixelFormatRGBA8Unorm},
        };
        const NSUInteger count = sizeof(rows) / sizeof(rows[0]);

        // The pattern bytes the lane builds: 01 02 03 .., cut to the row's length.
        uint8_t pattern[64];
        for (NSUInteger at = 0; at < sizeof(pattern); at += 1) { pattern[at] = (uint8_t)(at + 1); }

        for (NSUInteger at = 0; at < count; at += 1) {
            const Row row = rows[at];
            const NSUInteger length = row.bytesPerRow * row.height + 256;
            id<MTLBuffer> buffer = [device newBufferWithLength:length
                                                      options:MTLResourceStorageModeShared];
            memset(buffer.contents, kBefore, length);

            MTLTextureDescriptor *d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:row.format
                                                                  width:row.width
                                                                 height:row.height
                                                              mipmapped:NO];
            d.storageMode = MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            id<MTLTexture> texture = [buffer newTextureWithDescriptor:d
                                                              offset:0
                                                         bytesPerRow:row.bytesPerRow];
            if (texture == nil) {
                fprintf(stderr, "%lu %s: no texture\n", (unsigned long)row.format, row.fill);
                return 2;
            }

            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            SEL wanted = row.patternBytes == 0 ? colorSel : patternSel;
            if (![blit respondsToSelector:wanted]) {
                fprintf(stderr, "the blit encoder answers no %s\n", sel_getName(wanted));
                return 2;
            }
            if (at == 0) {
                printf("SELECTOR %s %s\n", sel_getName(patternSel),
                       [[(id)blit methodSignatureForSelector:patternSel] description].UTF8String);
                printf("SELECTOR %s %s\n", sel_getName(colorSel),
                       [[(id)blit methodSignatureForSelector:colorSel] description].UTF8String);
            }
            id<M2VPrivateFills> fills = (id<M2VPrivateFills>)blit;
            if (row.patternBytes == 0) {
                [fills fillTexture:texture
                             level:0
                             slice:0
                            region:row.region
                             color:MTLClearColorMake(0.25, 0.5, 0.75, 1.0)
                       pixelFormat:row.packedInto];
            } else {
                [fills fillTexture:texture
                             level:0
                             slice:0
                            region:row.region
                             bytes:pattern
                            length:row.patternBytes];
            }
            [blit endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "%lu %s: status %lu\n", (unsigned long)row.format, row.fill,
                        (unsigned long)commands.status);
                return 2;
            }

            printf("RUNS %lu %s", (unsigned long)row.format, row.fill);
            printRuns((const uint8_t *)buffer.contents, length);
            printf("\n");
        }
        printf("DONE\n");
    }
    return 0;
}
