// WHICH TILES a sparse MOVE past the texture's bounds actually moves, read off the TEXELS.
//
// `moveTextureMappingsFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:
// destinationSlice:destinationLevel:destinationOrigin:` takes a source level, a source slice and
// a source size past the texture's own: `probes/sparse_mapping_past_the_bounds.m` measured that
// much -- the command buffer completes with no error and no heap's `usedSize` moves. What it could
// not say, and said so, is WHICH tiles moved: a move changes no `usedSize` whatever it does, so
// that probe's only reading is blind to the whole question. The runtime's `sparse/encoder.rs` has
// carried "which tiles it moved there is not measured" ever since.
//
// This probe reads TEXELS either side of the move, so the candidate rules separate:
//
//   clip       the in-bounds part of the stated region moves, paired in order, and the rest of
//              the region does not: the destination's in-bounds tiles whose source lies outside
//              take nothing.
//   nothing    no tile moves at all; both textures read exactly as they did before.
//   wrap       a tile past a level's row continues into the next row (or a slice past the
//              texture's into slice 0, a level past it into level 0), so a tile the clip rule
//              drops lands somewhere anyway.
//   zeroed     the in-bounds part moves AND the destination tiles the move named but did not
//              fill are zeroed, rather than left holding what they held.
//
// Separating `zeroed` from "left untouched" needs the destination's overhang to hold a KNOWN
// NONZERO word before the move, which an unmapped tile cannot: an unmapped tile reads zero either
// way. That is what the `region-mapped` shape is for, and it is the one shape where the move's
// destination row is partly mapped.
//
// Every word this probe writes is nonzero and is neither 0 nor 0xFFFFFFFF, which are the two
// readings a texel can have for a reason other than a write of ours
// (`probes/sparse_unmapped_blit.m` on this chip: an unmapped tile reads 0 through a blit, a
// freshly mapped tile and a fresh Private texture read 0xFFFFFFFF). Each tile of each texture gets
// a word that names the texture, the slice and the tile, so a tile's provenance is read off its
// own value rather than inferred.
//
// WHY A PROBE AND NOT A CONFORMANCE CASE. The suite's sparse family bounds every region it states
// by the texture's own tile grid on purpose (`every_region_lies_inside_its_level`), and the whole
// question here is what lies past that bound. The shapes Metal's own API validation layer refuses
// are measured here, against plain Metal, and the one of them a case can carry safely afterwards
// is promoted into the family.
//
// RUN IT WITHOUT MTL_DEBUG_LAYER. Metal's validation refuses all four shapes and its default
// error mode ends the process; plain Metal is the behavioural oracle. The probe prints the
// device's own class name, so a run that got `MTLDebugDevice` instead of the real one is visible
// in its own output rather than assumed away.
//
// ONE SHAPE PER PROCESS, so a fault is attributable to the shape that caused it.
//
//   clang -fobjc-arc -fmodules -framework Metal -framework Foundation \
//       -o /tmp/sparse_move sparse_move_past_the_bounds.m
//   for s in control region-open region-mapped region-dest slice level dest-2d dest-row \
//            src-clip-2d; do /tmp/sparse_move $s; done
//
// THE SHAPES
//
//   control        an entirely IN-BOUNDS move: tile (0,0) of texture A to tile (3,3) of texture
//                  B. Its reading MUST move -- B(3,3) reading A(0,0)'s word and A(0,0) reading 0
//                  -- or nothing below is a measurement of anything. A control that cannot move
//                  proves nothing.
//   region-open    source origin (2,0), source size 4 by 1 where level 0 holds 4 by 4 tiles, so
//                  source tiles 2 and 3 are inside and 4 and 5 are not; destination origin (0,0)
//                  of B, whose whole row is UNMAPPED. `clip` reads B = [A2, A3, 0, 0]; `nothing`
//                  reads B = [0, 0, 0, 0]; `wrap` reads B = [A2, A3, A(0,1), A(1,1)].
//   region-mapped  the same move onto a B whose tiles (2,0) and (3,0) are MAPPED and hold their
//                  own words. `zeroed` reads those two as 0; every other rule leaves them holding
//                  B's own words. (A move onto a mapped tile moves nothing, which is Metal's own
//                  documented rule, so this shape says nothing about the first two tiles that
//                  `region-open` does not.)
//   region-dest    the overhang the other way round: source origin (0,0) size 4 by 1, all four
//                  tiles inside; destination origin (2,0), so destination tiles 2 and 3 are inside
//                  and 4 and 5 are not. `clip` reads B = [0, 0, A0, A1] with A2 and A3 still in
//                  place; `nothing` reads B all zero with A untouched; `wrap` puts A2 and A3 into
//                  B's next row.
//   slice          a 2-slice array A, both slices' level 0 filled, moved from source slice 2 --
//                  past the texture's two -- one tile, onto B slice 0. `nothing` leaves both of
//                  A's slices whole and B zero; a slice read as 0 reads B = A slice 0's word with
//                  A slice 0's tile now 0; a slice clamped to the last reads A slice 1's.
//   level          source level 9 of a 9-level texture (levels 0 to 8), one tile, onto B level 0.
//                  `nothing` leaves A's level 0 whole and B zero; a level read as 0 moves A's own
//                  level-0 tile.
//
// WHAT IT MEASURED, Apple M3 Max, macOS 27.0.1, plain Metal (`AGXG15CDevice`, read back off the
// device), one shape per process, every reading taken twice -- once per round of shapes -- and the
// same both times. Every command buffer `Completed` with no error and no heap's `usedSize` moved,
// which is the reading a counter can give and the reason it settles nothing.
//
// ONE RULE answers all eight shapes:
//
//   the SOURCE region is CLIPPED to the level's own tiles on each axis, and its surviving tiles
//   move in Metal's order (x, then y). The part outside moves nothing anywhere: it does not wrap
//   and it takes no destination tile. A source level or slice past the texture's own moves nothing
//   at all.
//
//   the DESTINATION is a RUN OF TILES, not a region. It starts at the destination origin, is laid
//   out `sourceSize.width` tiles to a row -- the width AS STATED, clipped only by the tiles the
//   level has to the right of the origin -- and takes the next row when one fills, one tile per
//   surviving source tile. So the destination's width is clipped and its height is NOT.
//
// Where the stated destination region fits inside the level, that row is the whole of it and the
// run is exactly the region, so the pairing is the tile's own offset within it. That is why the run
// shows only past an edge, and why `dest-2d` was the shape worth taking: without an in-bounds 2D
// reading, "the destination is the region" and "the destination is a run" could not be told apart.
//
// WHAT "TOOK" MEANS BELOW. A destination tile the move wrote did NOT read uniform: it printed
// `MIXED(first=<the source tile's word>)`. The source word is that tile's FIRST TEXEL, and the
// rest of the tile is not that word. So "B(3,3) took A(0,0)" below says WHICH source tile a
// destination tile took, read off its first texel, and says nothing about what the rest of that
// tile holds. This printout cannot say: its `differing>=` count stops at the first row of the tile
// that differs, so the count is a floor and never a share. Which tile each destination took is the
// whole question here, and the rule above rests on nothing else -- how much of a tile's bytes a
// move carries is a separate question, and one this probe does not read.
//
// Shape by shape:
//
//   control        B(3,3) took A(0,0), A(0,0) read zero, every other tile as it was.
//   region-open    source (2,0) 4x1: B(0,0) and B(1,0) took A(2,0) and A(3,0); B(2,0) and B(3,0)
//                  read zero and A's other rows were whole, so the two tiles past the level moved
//                  nowhere.
//   region-mapped  the same move onto a B holding its own words at (2,0) and (3,0): both still
//                  held them, so the destination tiles the overhang named are left as they were
//                  rather than zeroed.
//   region-dest    source (0,0) 4x1 onto (2,0): ALL FOUR moved -- B(2,0), B(3,0), B(2,1), B(3,1)
//                  took A(0,0), A(1,0), A(2,0), A(3,0) -- and A's whole row 0 read zero.
//   dest-row       the same onto (2,1): B(2,1), B(3,1), B(2,2), B(3,2). The run's row starts at the
//                  origin's own column, not at the level's left edge.
//   src-clip-2d    source (2,2) 4x4, clipped to 2x2, onto (1,1) with three tiles to its right:
//                  B(1,1), B(2,1), B(3,1), B(1,2) took A(2,2), A(3,2), A(2,3), A(3,3). This is the
//                  reading that says the row is the STATED width's: a row of the clipped two would
//                  have put the third tile at (1,2).
//   dest-2d        source (0,0) 2x2 onto (1,1), both sides inside: B(1,1), B(2,1), B(1,2), B(2,2)
//                  took A(0,0), A(1,0), A(0,1), A(1,1) -- the region's own offsets.
//   slice          source slice 2 of a two-slice texture: nothing moved, both slices whole.
//   level          source level 9 of a nine-level texture: nothing moved.
//
// Two things stay UNMEASURED and the runtime takes the conservative form for each: a run that
// would pass the level's LAST ROW (it drops those tiles rather than moving them somewhere nobody
// has read), and a region deeper than one image, which is the one part of the row rule a 3D move
// could read differently.
//
// With MTL_DEBUG_LAYER=1 the `region-open` shape read the same texels and Metal's validation
// logged `(region.origin.x + region.width)(6) must be <= widthInTiles(4).`, so the layer is what
// objects to these calls and plain Metal's behaviour is not the layer's. The device class is
// printed for exactly this reason: that run reported `MTLDebugDevice` and every other reported
// `AGXG15CDevice`.
//
//   dest-2d        2 by 2 source tiles at (0,0), all inside, onto destination origin (1,1), which
//                  is inside too and is NOT Z-aligned. The region rule reads B(1,1), B(2,1),
//                  B(1,2), B(2,2); the page-sequence rule reads B(1,1), B(2,0), B(3,0), B(2,1).
//   dest-row       4 by 1 source tiles at (0,0), all inside, onto destination origin (2,1), two
//                  tiles past the row. The region rule puts two at (2,1) and (3,1) and drops or
//                  wraps the rest; the page-sequence rule puts them at (2,1), (3,1), (0,2), (1,2).
//   src-clip-2d    the source clipping in TWO dimensions, which is the shape a pairing by list
//                  index gets wrong: 4 by 4 tiles at (2,2) of a level holding 4 by 4, so 2 by 2
//                  survive, onto destination origin (1,1). Pairing by the tile's place in the
//                  region reads B(1,1), B(2,1), B(1,2), B(2,2); pairing by list index reads
//                  B(1,1), B(2,1), B(3,1), B(0,2); the page sequence reads B(1,1), B(2,0),
//                  B(3,0), B(2,1).
//
// WHAT IT READS, per shape: the heap's `usedSize` before and after the move (which no rule above
// moves, and which is the reading that makes the blindness this probe is answering visible), the
// command buffer's status and error, and every tile of each texture's level 0 -- each tile's
// verdict read off ALL 4,096 of its texels, so a partly moved tile cannot read as a whole one.
//
// Nothing here dispatches, draws or loops over a GPU; every region in tiles is checked against the
// level's grid before the call except the one the shape is about, and that one is the measurement.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

static const NSUInteger kSide = 256;
static const NSUInteger kLevels = 9;
static const NSUInteger kHeapBytes = 4u << 20;
static const NSUInteger kTile = 64;      // texels a tile, at a 16-KiB page and R32Uint
static const NSUInteger kTiles = 4;      // tiles level 0 holds on each axis
static const NSUInteger kTexelBytes = 4;

static id<MTLDevice> gDevice = nil;
static id<MTLCommandQueue> gQueue = nil;
static id<MTLHeap> gHeap = nil;

/// The word tile (x, y) of slice `slice` of texture `which` ('A' or 'B') is filled with: nonzero,
/// never 0xFFFFFFFF, and unique per tile so a texel says where it came from.
///
/// The first form of this function OR'd `y << 8` into a constant that already held bit 8, so rows
/// 0 and 1 shared a word and rows 2 and 3 shared one, and a tile's provenance was only recoverable
/// by elimination. Each field has its own byte now.
static uint32_t WordOf(char which, NSUInteger slice, NSUInteger x, NSUInteger y) {
    uint32_t base = (which == 'A') ? 0xa1000000u : 0xb1000000u;
    return base | (uint32_t)(slice << 16) | (uint32_t)(y << 8) | (uint32_t)x;
}

/// A command buffer run to completion, with its status and error printed under `what`.
static void Run(id<MTLCommandBuffer> cb, const char *what) {
    [cb commit];
    [cb waitUntilCompleted];
    printf("  %-18s status = %lu error = %s\n", what, (unsigned long)cb.status,
           cb.error == nil ? "(none)" : cb.error.localizedDescription.UTF8String);
}

/// Map or unmap `size` tiles at (`x`, `y`) of `level` and `slice`, in a command buffer of its own.
static void MapTiles(id<MTLTexture> texture, MTLSparseTextureMappingMode mode, NSUInteger x,
                     NSUInteger y, NSUInteger w, NSUInteger h, NSUInteger level, NSUInteger slice,
                     const char *what) {
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLResourceStateCommandEncoder> e = [cb resourceStateCommandEncoder];
    [e updateTextureMapping:texture
                       mode:mode
                     region:MTLRegionMake3D(x, y, 0, w, h, 1)
                   mipLevel:level
                      slice:slice];
    [e endEncoding];
    Run(cb, what);
}

/// Write `word` into every texel of tile (`x`, `y`) of level 0 of `slice`, out of a Shared buffer
/// of its own. The tile must be mapped: a blit over an unmapped sparse tile is dropped on this
/// chip (`probes/sparse_unmapped_blit.m`), which is why each fill names one tile rather than a
/// whole level -- a whole-level fill of a partly mapped texture would leave the dropped tiles
/// reading zero and "nothing moved" would be indistinguishable from "the fill never reached it".
static void FillTile(id<MTLTexture> texture, NSUInteger slice, NSUInteger x, NSUInteger y,
                     uint32_t word) {
    NSUInteger words = kTile * kTile;
    id<MTLBuffer> staging = [gDevice newBufferWithLength:words * kTexelBytes
                                                 options:MTLResourceStorageModeShared];
    uint32_t *at = (uint32_t *)staging.contents;
    for (NSUInteger i = 0; i < words; i += 1) {
        at[i] = word;
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:staging
            sourceOffset:0
       sourceBytesPerRow:kTile * kTexelBytes
     sourceBytesPerImage:words * kTexelBytes
              sourceSize:MTLSizeMake(kTile, kTile, 1)
               toTexture:texture
        destinationSlice:slice
        destinationLevel:0
       destinationOrigin:MTLOriginMake(x * kTile, y * kTile, 0)];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        printf("  FILL FAILED status = %lu\n", (unsigned long)cb.status);
    }
}

/// Read every texel of level 0 of `slice` back and print one verdict a tile, off ALL of its texels.
static void ReadLevel(id<MTLTexture> texture, NSUInteger slice, const char *name) {
    NSUInteger words = kSide * kSide;
    id<MTLBuffer> into = [gDevice newBufferWithLength:words * kTexelBytes
                                             options:MTLResourceStorageModeShared];
    // A guard the GPU must overwrite: a reading that is still the guard says the copy never ran.
    memset(into.contents, 0x5a, words * kTexelBytes);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:texture
              sourceSlice:slice
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(kSide, kSide, 1)
                 toBuffer:into
        destinationOffset:0
   destinationBytesPerRow:kSide * kTexelBytes
 destinationBytesPerImage:words * kTexelBytes];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        printf("  READ FAILED %s slice %lu status = %lu\n", name, (unsigned long)slice,
               (unsigned long)cb.status);
        return;
    }
    const uint32_t *at = (const uint32_t *)into.contents;
    for (NSUInteger ty = 0; ty < kTiles; ty += 1) {
        printf("  %s slice %lu row %lu:", name, (unsigned long)slice, (unsigned long)ty);
        for (NSUInteger tx = 0; tx < kTiles; tx += 1) {
            uint32_t first = at[(ty * kTile) * kSide + tx * kTile];
            BOOL uniform = YES;
            NSUInteger differing = 0;
            for (NSUInteger y = 0; y < kTile && uniform; y += 1) {
                for (NSUInteger x = 0; x < kTile; x += 1) {
                    if (at[(ty * kTile + y) * kSide + tx * kTile + x] != first) {
                        uniform = NO;
                        differing += 1;
                    }
                }
            }
            if (uniform) {
                printf(" 0x%08x", first);
            } else {
                printf(" MIXED(first=0x%08x,differing>=%lu)", first, (unsigned long)differing);
            }
        }
        printf("\n");
    }
}

static id<MTLTexture> MakeTexture(NSUInteger slices) {
    MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
    td.textureType = slices > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    td.pixelFormat = MTLPixelFormatR32Uint;
    td.width = kSide;
    td.height = kSide;
    td.mipmapLevelCount = kLevels;
    td.arrayLength = slices;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    // Every descriptor field is read back off the descriptor object before the call, so a field
    // the probe believes it set and did not is visible here rather than in a vacuous reading.
    printf("  descriptor: type=%lu format=%lu %lux%lu levels=%lu slices=%lu storage=%lu usage=%lu\n",
           (unsigned long)td.textureType, (unsigned long)td.pixelFormat, (unsigned long)td.width,
           (unsigned long)td.height, (unsigned long)td.mipmapLevelCount,
           (unsigned long)td.arrayLength, (unsigned long)td.storageMode, (unsigned long)td.usage);
    return [gHeap newTextureWithDescriptor:td];
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr,
                "usage: %s control|region-open|region-mapped|region-dest|slice|level|dest-2d|"
                "dest-row|src-clip-2d\n",
                argv[0]);
        return 2;
    }
    const char *shape = argv[1];
    @autoreleasepool {
        gDevice = MTLCreateSystemDefaultDevice();
        if (gDevice == nil) {
            fprintf(stderr, "no Metal device\n");
            return 2;
        }
        // Which device class answered: `MTLDebugDevice` says the validation layer is on, and this
        // probe's oracle is plain Metal.
        printf("shape=%s device=%s class=%s\n", shape, gDevice.name.UTF8String,
               object_getClassName(gDevice));

        BOOL array = strcmp(shape, "slice") == 0;
        NSUInteger slices = array ? 2 : 1;

        MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
        hd.type = MTLHeapTypeSparse;
        hd.storageMode = MTLStorageModePrivate;
        hd.sparsePageSize = MTLSparsePageSize16;
        hd.size = kHeapBytes;
        gHeap = [gDevice newHeapWithDescriptor:hd];
        gQueue = [gDevice newCommandQueue];
        if (gHeap == nil || gQueue == nil) {
            fprintf(stderr, "no sparse heap or queue\n");
            return 2;
        }
        id<MTLTexture> a = MakeTexture(slices);
        id<MTLTexture> b = MakeTexture(slices);
        if (a == nil || b == nil) {
            fprintf(stderr, "no sparse texture\n");
            return 2;
        }
        MTLSize tile = [gDevice sparseTileSizeWithTextureType:a.textureType
                                                 pixelFormat:MTLPixelFormatR32Uint
                                                 sampleCount:1
                                              sparsePageSize:MTLSparsePageSize16];
        printf("  tile=%lux%lux%lu firstMipmapInTail=%lu tileBytes=%lu\n",
               (unsigned long)tile.width, (unsigned long)tile.height, (unsigned long)tile.depth,
               (unsigned long)a.firstMipmapInTail, (unsigned long)gDevice.sparseTileSizeInBytes);
        if (tile.width != kTile || tile.height != kTile) {
            fprintf(stderr, "the device's tile is not the grid this probe states\n");
            return 2;
        }

        // Texture A: every tile of level 0 of every slice mapped and filled with its own word.
        for (NSUInteger slice = 0; slice < slices; slice += 1) {
            MapTiles(a, MTLSparseTextureMappingModeMap, 0, 0, kTiles, kTiles, 0, slice, "map A");
            for (NSUInteger y = 0; y < kTiles; y += 1) {
                for (NSUInteger x = 0; x < kTiles; x += 1) {
                    FillTile(a, slice, x, y, WordOf('A', slice, x, y));
                }
            }
        }
        // Texture B: unmapped, except in `region-mapped`, where the destination's overhang holds
        // its own words so "zeroed" and "left untouched" read differently.
        if (strcmp(shape, "region-mapped") == 0) {
            for (NSUInteger x = 2; x < 4; x += 1) {
                MapTiles(b, MTLSparseTextureMappingModeMap, x, 0, 1, 1, 0, 0, "map B");
                FillTile(b, 0, x, 0, WordOf('B', 0, x, 0));
            }
        }

        printf("BEFORE usedSize = %lu\n", (unsigned long)gHeap.usedSize);
        for (NSUInteger slice = 0; slice < slices; slice += 1) {
            ReadLevel(a, slice, "A");
        }
        ReadLevel(b, 0, "B");
        fflush(stdout);

        // The move the shape is about. Every number is stated here, in tiles.
        NSUInteger srcSlice = 0, srcLevel = 0, dstSlice = 0, dstLevel = 0;
        MTLOrigin srcOrigin = MTLOriginMake(0, 0, 0);
        MTLSize srcSize = MTLSizeMake(1, 1, 1);
        MTLOrigin dstOrigin = MTLOriginMake(3, 3, 0);
        if (strcmp(shape, "control") == 0) {
            // In bounds both sides: the reading that must move.
        } else if (strcmp(shape, "region-open") == 0 || strcmp(shape, "region-mapped") == 0) {
            srcOrigin = MTLOriginMake(2, 0, 0);
            srcSize = MTLSizeMake(4, 1, 1);
            dstOrigin = MTLOriginMake(0, 0, 0);
        } else if (strcmp(shape, "region-dest") == 0) {
            srcOrigin = MTLOriginMake(0, 0, 0);
            srcSize = MTLSizeMake(4, 1, 1);
            dstOrigin = MTLOriginMake(2, 0, 0);
        } else if (strcmp(shape, "slice") == 0) {
            srcSlice = 2;
            dstOrigin = MTLOriginMake(0, 0, 0);
        } else if (strcmp(shape, "level") == 0) {
            srcLevel = kLevels;
            dstOrigin = MTLOriginMake(0, 0, 0);
        } else if (strcmp(shape, "dest-2d") == 0) {
            srcOrigin = MTLOriginMake(0, 0, 0);
            srcSize = MTLSizeMake(2, 2, 1);
            dstOrigin = MTLOriginMake(1, 1, 0);
        } else if (strcmp(shape, "dest-row") == 0) {
            srcOrigin = MTLOriginMake(0, 0, 0);
            srcSize = MTLSizeMake(4, 1, 1);
            dstOrigin = MTLOriginMake(2, 1, 0);
        } else if (strcmp(shape, "src-clip-2d") == 0) {
            srcOrigin = MTLOriginMake(2, 2, 0);
            srcSize = MTLSizeMake(4, 4, 1);
            dstOrigin = MTLOriginMake(1, 1, 0);
        } else {
            fprintf(stderr, "unknown shape %s\n", shape);
            return 2;
        }
        printf("MOVE A slice %lu level %lu origin (%lu,%lu) size %lux%lu -> B slice %lu level %lu "
               "origin (%lu,%lu)\n",
               (unsigned long)srcSlice, (unsigned long)srcLevel, (unsigned long)srcOrigin.x,
               (unsigned long)srcOrigin.y, (unsigned long)srcSize.width,
               (unsigned long)srcSize.height, (unsigned long)dstSlice, (unsigned long)dstLevel,
               (unsigned long)dstOrigin.x, (unsigned long)dstOrigin.y);
        fflush(stdout);

        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLResourceStateCommandEncoder> e = [cb resourceStateCommandEncoder];
        [e moveTextureMappingsFromTexture:a
                             sourceSlice:srcSlice
                             sourceLevel:srcLevel
                            sourceOrigin:srcOrigin
                              sourceSize:srcSize
                               toTexture:b
                        destinationSlice:dstSlice
                        destinationLevel:dstLevel
                       destinationOrigin:dstOrigin];
        [e endEncoding];
        Run(cb, "move");

        printf("AFTER usedSize = %lu\n", (unsigned long)gHeap.usedSize);
        for (NSUInteger slice = 0; slice < slices; slice += 1) {
            ReadLevel(a, slice, "A");
        }
        ReadLevel(b, 0, "B");
        fflush(stdout);
    }
    return 0;
}
