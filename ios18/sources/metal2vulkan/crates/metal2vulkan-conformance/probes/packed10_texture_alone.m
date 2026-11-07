// Whether `newTextureWithDescriptor:` gives a texture for `MTLPixelFormatR10Unorm_PACKED` and
// `MTLPixelFormatRG10Unorm_PACKED` from a descriptor alone, and whether its answer turns on the
// descriptor's storage mode or its usage.
//
// The question needs asking because a storage mode's nil and a format's nil look identical at the
// call. `newSharedTextureWithDescriptor:` answers nil for EVERY format outside `Private` storage,
// and a probe that read one of those nils as a format's produced a wrong rule for these very
// formats. So every cell here names its format, its storage mode and its usage in its own output
// line, and every cell runs beside two controls in the same mode and usage:
//
//   * `RGBA8Unorm` (70), which every device makes in every mode the mode itself allows, and
//   * `R10Unorm_X6` (576), the padded 10-bit format that IS made, so that "padded 10-bit" cannot
//     stand in for "this format".
//
// A cell whose controls answer nil says nothing about the subject: the mode or the usage refused
// it, and the line is marked `CONTROLS-NIL`. Only a cell whose controls both answer a texture can
// report `NO-TEXTURE` or `MADE` for the subject.
//
// One cell per process: Metal's descriptor validation ends the process for some combinations
// (a memoryless texture of no render-target usage among them), and an ended process must not take
// the other cells with it. The driver loop in the header below runs each cell in a fresh process
// and repeats it, so determinism is a count rather than an impression.
//
// The probe allocates textures and reads no memory it did not allocate; nothing is encoded or
// submitted, so no step can fault the GPU.
//
// clang -fobjc-arc -Wno-deprecated-declarations -framework Metal -framework Foundation \
//     -o packed10_texture_alone packed10_texture_alone.m
//
// (`MTLStorageModeManaged` is deprecated on macOS 27; the probe still asks it, because a mode's
// own nil is exactly what has to be told apart from a format's.)
//
//   for run in 1 2 3 4; do
//     for storage in private shared managed memoryless; do
//       for usage in read read-write read-target target; do
//         ./packed10_texture_alone "$storage" "$usage"
//       done
//     done
//   done

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const NSUInteger kR10UnormPACKED = 588;
static const NSUInteger kRG10UnormPACKED = 589;
static const NSUInteger kR10UnormX6 = 576;
static const NSUInteger kRGBA8Unorm = 70;

struct named {
    const char *name;
    NSUInteger value;
};

static const struct named kStorage[] = {
    {"private", MTLStorageModePrivate},
    {"shared", MTLStorageModeShared},
    {"managed", MTLStorageModeManaged},
    {"memoryless", MTLStorageModeMemoryless},
};

static const struct named kUsage[] = {
    {"read", MTLTextureUsageShaderRead},
    {"read-write", MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite},
    {"read-target", MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget},
    {"target", MTLTextureUsageRenderTarget},
};

static const struct named *lookup(const struct named *table, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/// Whether the device makes a 2D 8x8 texture of `format` in `storage` under `usage`, through
/// `newTextureWithDescriptor:` alone.
static BOOL makes(id<MTLDevice> device, NSUInteger format, NSUInteger storage, NSUInteger usage) {
    MTLTextureDescriptor *d = [[MTLTextureDescriptor alloc] init];
    d.textureType = MTLTextureType2D;
    d.pixelFormat = (MTLPixelFormat)format;
    d.width = 8;
    d.height = 8;
    d.depth = 1;
    d.mipmapLevelCount = 1;
    d.sampleCount = 1;
    d.arrayLength = 1;
    d.usage = (MTLTextureUsage)usage;
    d.storageMode = (MTLStorageMode)storage;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    return texture != nil;
}

int main(int argc, const char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: packed10_texture_alone <storage> <usage>\n");
        fprintf(stderr, "  storage: private shared managed memoryless\n");
        fprintf(stderr, "  usage:   read read-write read-target target\n");
        return 2;
    }
    const struct named *storage =
        lookup(kStorage, sizeof kStorage / sizeof *kStorage, argv[1]);
    const struct named *usage = lookup(kUsage, sizeof kUsage / sizeof *kUsage, argv[2]);
    if (storage == NULL || usage == NULL) {
        fprintf(stderr, "packed10_texture_alone: unknown storage or usage\n");
        return 2;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            fprintf(stderr, "packed10_texture_alone: no Metal device\n");
            return 2;
        }
        BOOL rgba8 = makes(device, kRGBA8Unorm, storage->value, usage->value);
        BOOL x6 = makes(device, kR10UnormX6, storage->value, usage->value);
        BOOL r10 = makes(device, kR10UnormPACKED, storage->value, usage->value);
        BOOL rg10 = makes(device, kRG10UnormPACKED, storage->value, usage->value);
        BOOL controls = rgba8 && x6;
        // Each cell names what it varied, so no later reader has to guess which knob moved.
        printf("device=%s storage=%s usage=%s(0x%lx) "
               "control:RGBA8Unorm=%s control:R10Unorm_X6=%s "
               "R10Unorm_PACKED=%s RG10Unorm_PACKED=%s verdict=%s\n",
               device.name.UTF8String, storage->name, usage->name,
               (unsigned long)usage->value, rgba8 ? "MADE" : "nil", x6 ? "MADE" : "nil",
               r10 ? "MADE" : "nil", rg10 ? "MADE" : "nil",
               !controls ? "CONTROLS-NIL"
                         : (r10 || rg10 ? "MADE-SUBJECT" : "NO-TEXTURE-SUBJECT"));
    }
    return 0;
}
