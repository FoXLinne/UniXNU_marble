// What plain Metal's heap `usedSize` does for an INDIRECT sparse mapping update whose slice lies
// past the texture's own slices, through
// `updateTextureMapping:mode:indirectBuffer:indirectBufferOffset:`.
//
// Why a probe and not a conformance case: Metal's API validation layer refuses an out-of-bounds
// slice through the direct selector, and the suite's sparse family bounds every region it states
// by the texture's tile grid on purpose. Through the INDIRECT selector the arguments live in GPU
// memory, so the validation layer has nothing to read and cannot object at all; what settles the
// behaviour is what plain Metal does with them.
//
// `sparse/encoder.rs` carried three claims about this reading, none of them reproducible from
// anything in the tree (the only committed probe, `sparse_mapping_past_the_bounds.m`, runs the
// DIRECT selector): that the mapping takes a tile more BESIDE OTHER REGIONS, NONE ALONE, and that
// an UNMAP of it can take the count BELOW ZERO. This probe measures all three separately.
//
// Build and run (one shape per process, so a configuration is never perturbed by another's
// mapping, and a fault is attributable to the shape that caused it):
//
//   clang -fobjc-arc -framework Metal -framework Foundation \
//       -o sparse_indirect_usedsize sparse_indirect_usedsize.m
//   for shape in direct-slice-alone indirect-control indirect-two-inbounds \
//                indirect-slice-alone indirect-slice-beside indirect-slice-after-four \
//                indirect-unmap-slice-alone indirect-unmap-slice-after-one \
//                indirect-unmap-slice-after-four indirect-unmap-slice-beside \
//                indirect-map-then-unmap-slice; do ./sparse_indirect_usedsize $shape; done
//
// Run it WITHOUT MTL_DEBUG_LAYER. Plain Metal is the behavioural oracle, and on this machine the
// layer has been measured to change what a shader reads, so a layer-on run is a separate
// observation about what Metal's layer objects to and never a control.
//
// Every shape reads the device's name back off the object, prints `usedSize` TWICE at each step
// (a counter that answers two different values at one step is not a counter), and ends with an
// IN-BOUNDS CONTROL through the same selector it measured -- run after the measurement so that
// an "alone" configuration is still alone when it is read. A shape whose control does not move
// the count has measured nothing, whatever its own reading said.
//
// The shapes, and what each asks:
//
//   direct-slice-alone          the DIRECT selector, slice 2 of a 2-slice texture, nothing else
//                               mapped. This reproduces the committed reading (usedSize 0 ->
//                               16,384) off `sparse_mapping_past_the_bounds.m`; if it does not,
//                               this harness is wrong and not the record.
//   indirect-control            one in-bounds tile mapped indirectly, then unmapped indirectly.
//                               The pure harness proof for both directions of the count.
//   indirect-two-inbounds       one indirect update of two in-bounds tiles: the control for the
//                               count=2 path that `indirect-slice-beside` uses.
//   indirect-slice-alone        one indirect update of ONE region, slice 2, nothing else mapped.
//                               Sub-claim 1 and the "none alone" half of sub-claim 2.
//   indirect-slice-beside       one indirect update of TWO regions, an in-bounds tile and the
//                               slice-2 region. Against `indirect-two-inbounds` and
//                               `indirect-control` this is the "a tile more beside other
//                               regions" half of sub-claim 2.
//   indirect-slice-after-four   four in-bounds tiles mapped first, then the slice-2 region alone
//                               indirectly: "beside other regions" read as other mappings
//                               already standing rather than as siblings in one argument list.
//   indirect-unmap-slice-alone  nothing mapped, usedSize 0, an indirect UNMAP of slice 2. If the
//                               count can go below zero this is where it shows: `usedSize` is an
//                               NSUInteger, so below zero reads as a value near 2^64.
//   indirect-unmap-slice-after-one    one tile mapped, then the indirect unmap of slice 2.
//   indirect-unmap-slice-after-four   four tiles mapped, then the indirect unmap of slice 2.
//                               This is the configuration another reading found did NOT lower
//                               the count; it is one configuration, so all of them are here.
//   indirect-unmap-slice-beside one tile mapped, then ONE indirect unmap of two regions, that
//                               tile and the slice-2 region: two tiles taken off a count of one
//                               would be the below-zero path through a real unmap.
//   indirect-map-then-unmap-slice  from 0: the indirect map of slice 2, read; then the indirect
//                               unmap of the same region, read. Whatever the map took, the unmap
//                               has to give back, or the count drifts.
//
// Once the first reading showed the region DOES take a page, these bound what it takes, which is
// what an exact form of it in the runtime would have to reproduce:
//
//   indirect-slice-twice        the same out-of-bounds region mapped twice, in two command
//                               buffers. One page in total is a charge per REGION, like a tile
//                               already mapped; two is a page leaked per call.
//   indirect-slice-unmap-twice  map it, unmap it, unmap it AGAIN. This is the sharpest test of
//                               the below-zero claim: a second unmap taking another page off a
//                               count that already gave the first one back is the only way the
//                               count could pass below where it started.
//   indirect-two-oob-slices     one update naming slice 2 AND slice 3: whether the charge is per
//                               out-of-bounds region or one for the update however many it holds.
//   indirect-slice-big-region   slice 2 with 8 by 8 tiles rather than one: whether the charge
//                               follows the region's size or is one page whatever it states.
//   indirect-level-alone        a LEVEL past the texture's (level 9 of nine), in bounds on the
//                               slice: whether the charge is the slice axis' alone.
//   indirect-origin-alone       an origin past the level's last tile, (4, 4) of a 4 by 4 level:
//                               the other out-of-bounds shape that maps nothing.
//
// The texture is the one the suite's sparse family uses, as a 2-slice array so that slice 2 is
// past its own: 256 by 256 R32Float, nine levels, which at a 16-KiB page is a 64 by 64 texel
// tile, so level 0 holds 4 by 4 tiles, level 1 holds 2 by 2, level 2 holds one and
// `firstMipmapInTail` is 3. The heap is 4 MiB, far more than the tiles any shape maps.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const NSUInteger kSide = 256;
static const NSUInteger kLevels = 9;
static const NSUInteger kSlices = 2;
static const NSUInteger kHeapBytes = 4u << 20;
static const NSUInteger kArgumentWords = 8;

// One `MTLMapIndirectArguments` at `index` of the argument list `words` holds (the SDK's
// MTLResourceStateCommandEncoder.h): the region's origin and size in tiles, then the level and
// the slice, each a uint32. `words[0]` is the count.
static void state_region(uint32_t *words, NSUInteger index, uint32_t ox, uint32_t oy, uint32_t oz,
                         uint32_t width, uint32_t height, uint32_t depth, uint32_t level,
                         uint32_t slice) {
    uint32_t *argument = words + 1 + index * kArgumentWords;
    argument[0] = ox;
    argument[1] = oy;
    argument[2] = oz;
    argument[3] = width;
    argument[4] = height;
    argument[5] = depth;
    argument[6] = level;
    argument[7] = slice;
}

// `usedSize` twice, so that a step whose two readings disagree is visible as such.
static void report(const char *step, id<MTLHeap> heap) {
    unsigned long long first = (unsigned long long)heap.usedSize;
    unsigned long long again = (unsigned long long)heap.usedSize;
    printf("%-34s usedSize = %llu (again %llu)%s\n", step, first, again,
           first == again ? "" : "  <-- THE TWO READINGS DISAGREE");
    fflush(stdout);
}

static void finish(const char *step, id<MTLCommandBuffer> cb, id<MTLHeap> heap) {
    [cb commit];
    [cb waitUntilCompleted];
    printf("%-34s status = %lu error = %s\n", step, (unsigned long)cb.status,
           cb.error == nil ? "(none)" : cb.error.localizedDescription.UTF8String);
    report(step, heap);
}

// The direct selector over one region, in a command buffer of its own.
static void direct(const char *step, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                   id<MTLHeap> heap, MTLSparseTextureMappingMode mode, MTLRegion region,
                   NSUInteger level, NSUInteger slice) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLResourceStateCommandEncoder> encoder = [cb resourceStateCommandEncoder];
    [encoder updateTextureMapping:texture mode:mode region:region mipLevel:level slice:slice];
    [encoder endEncoding];
    finish(step, cb, heap);
}

// The indirect selector over the argument list already written into `arguments`, in a command
// buffer of its own.
static void indirect(const char *step, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                     id<MTLHeap> heap, MTLSparseTextureMappingMode mode,
                     id<MTLBuffer> arguments) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLResourceStateCommandEncoder> encoder = [cb resourceStateCommandEncoder];
    [encoder updateTextureMapping:texture
                             mode:mode
                   indirectBuffer:arguments
             indirectBufferOffset:0];
    [encoder endEncoding];
    finish(step, cb, heap);
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <shape>  (the shape names are in this file's header)\n",
                argv[0]);
        return 2;
    }
    const char *shape = argv[1];
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            fprintf(stderr, "no Metal device\n");
            return 2;
        }

        MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
        td.textureType = MTLTextureType2DArray;
        td.pixelFormat = MTLPixelFormatR32Float;
        td.width = kSide;
        td.height = kSide;
        td.mipmapLevelCount = kLevels;
        td.arrayLength = kSlices;
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

        MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
        hd.type = MTLHeapTypeSparse;
        hd.storageMode = MTLStorageModePrivate;
        hd.sparsePageSize = MTLSparsePageSize16;
        hd.size = kHeapBytes;
        id<MTLHeap> heap = [device newHeapWithDescriptor:hd];
        if (heap == nil) {
            fprintf(stderr, "no sparse heap\n");
            return 2;
        }
        id<MTLTexture> texture = [heap newTextureWithDescriptor:td];
        if (texture == nil) {
            fprintf(stderr, "no sparse texture\n");
            return 2;
        }

        // The device class read back off the object, so the reading names the machine it came
        // from rather than the machine somebody believed it came from.
        MTLSize tile = [device sparseTileSizeWithTextureType:td.textureType
                                                pixelFormat:td.pixelFormat
                                                sampleCount:1
                                             sparsePageSize:MTLSparsePageSize16];
        printf("shape=%s device=\"%s\" layer=%s\n", shape, device.name.UTF8String,
               getenv("MTL_DEBUG_LAYER") == NULL ? "off" : getenv("MTL_DEBUG_LAYER"));
        printf("slices=%lu levels=%lu tile=%lux%lux%lu tileBytes=%lu firstMipmapInTail=%lu\n",
               (unsigned long)texture.arrayLength, (unsigned long)texture.mipmapLevelCount,
               (unsigned long)tile.width, (unsigned long)tile.height, (unsigned long)tile.depth,
               (unsigned long)device.sparseTileSizeInBytes,
               (unsigned long)texture.firstMipmapInTail);
        fflush(stdout);

        id<MTLCommandQueue> queue = [device newCommandQueue];

        // Room for a count and two regions; every shape writes its own list before it runs.
        id<MTLBuffer> arguments =
            [device newBufferWithLength:4 * (1 + 2 * kArgumentWords)
                                options:MTLResourceStorageModeShared];
        uint32_t *words = (uint32_t *)arguments.contents;
        memset(words, 0, arguments.length);

        const MTLRegion one_tile = MTLRegionMake3D(0, 0, 0, 1, 1, 1);
        const NSUInteger past_the_slices = kSlices;  // slice 2 of a 2-slice texture

        report("start", heap);

        if (strcmp(shape, "direct-slice-alone") == 0) {
            direct("direct map slice 2", queue, texture, heap, MTLSparseTextureMappingModeMap,
                   one_tile, 0, past_the_slices);
            direct("control: direct map (0,0)", queue, texture, heap,
                   MTLSparseTextureMappingModeMap, one_tile, 0, 0);
        } else if (strcmp(shape, "indirect-control") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("indirect map (0,0)", queue, texture, heap, MTLSparseTextureMappingModeMap,
                     arguments);
            indirect("indirect unmap (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
        } else if (strcmp(shape, "indirect-two-inbounds") == 0) {
            words[0] = 2;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            state_region(words, 1, 1, 0, 0, 1, 1, 1, 0, 0);
            indirect("indirect map (0,0) and (1,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            words[0] = 1;
            state_region(words, 0, 2, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (2,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-slice-alone") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-slice-beside") == 0) {
            words[0] = 2;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            state_region(words, 1, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map (0,0) and slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            words[0] = 1;
            state_region(words, 0, 1, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (1,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-slice-after-four") == 0) {
            direct("direct map 2x2 tiles", queue, texture, heap, MTLSparseTextureMappingModeMap,
                   MTLRegionMake3D(0, 0, 0, 2, 2, 1), 0, 0);
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            state_region(words, 0, 3, 3, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (3,3)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-unmap-slice-alone") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect UNMAP slice 2, at 0", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            indirect("control: indirect unmap (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
        } else if (strcmp(shape, "indirect-unmap-slice-after-one") == 0) {
            direct("direct map (0,0)", queue, texture, heap, MTLSparseTextureMappingModeMap,
                   one_tile, 0, 0);
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect UNMAP slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect unmap (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
        } else if (strcmp(shape, "indirect-unmap-slice-after-four") == 0) {
            direct("direct map 2x2 tiles", queue, texture, heap, MTLSparseTextureMappingModeMap,
                   MTLRegionMake3D(0, 0, 0, 2, 2, 1), 0, 0);
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect UNMAP slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect unmap (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
        } else if (strcmp(shape, "indirect-unmap-slice-beside") == 0) {
            direct("direct map (0,0)", queue, texture, heap, MTLSparseTextureMappingModeMap,
                   one_tile, 0, 0);
            words[0] = 2;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            state_region(words, 1, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect UNMAP (0,0) and slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            words[0] = 1;
            state_region(words, 0, 1, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (1,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-map-then-unmap-slice") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            indirect("indirect UNMAP slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-slice-twice") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            indirect("indirect map slice 2 AGAIN", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-slice-unmap-twice") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            indirect("indirect UNMAP slice 2", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            indirect("indirect UNMAP slice 2 AGAIN", queue, texture, heap,
                     MTLSparseTextureMappingModeUnmap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-two-oob-slices") == 0) {
            words[0] = 2;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices);
            state_region(words, 1, 0, 0, 0, 1, 1, 1, 0, (uint32_t)past_the_slices + 1);
            indirect("indirect map slices 2 and 3", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-slice-big-region") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 8, 8, 1, 0, (uint32_t)past_the_slices);
            indirect("indirect map slice 2, 8x8 tiles", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-level-alone") == 0) {
            words[0] = 1;
            state_region(words, 0, 0, 0, 0, 1, 1, 1, (uint32_t)kLevels, 0);
            indirect("indirect map level 9", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else if (strcmp(shape, "indirect-origin-alone") == 0) {
            words[0] = 1;
            state_region(words, 0, 4, 4, 0, 1, 1, 1, 0, 0);
            indirect("indirect map origin (4,4)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
            state_region(words, 0, 0, 0, 0, 1, 1, 1, 0, 0);
            indirect("control: indirect map (0,0)", queue, texture, heap,
                     MTLSparseTextureMappingModeMap, arguments);
        } else {
            fprintf(stderr, "unknown shape %s\n", shape);
            return 2;
        }
    }
    return 0;
}
