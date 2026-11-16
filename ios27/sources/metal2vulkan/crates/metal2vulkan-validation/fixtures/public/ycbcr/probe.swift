// The probe that recorded metal_m3_max.bin. Owned code, written for this repository.
// Build and run it with
//   xcrun swiftc -O probe.swift -o probe && ./probe <out.bin>
// It makes a 40 x 24 '420v' IOSurface laid out as the ycbcr_textures lane lays its stand-in
// buffer out (the luma plane at byte 0, rows 64 bytes apart, the interleaved chroma plane at
// byte 1536, rows 64 bytes apart), fills it with the lane's generator at seed 5, makes a
// MTLPixelFormatYCBCR8_420_2P (500) texture over plane 0, reads every texel in a kernel and
// writes each texel's red, green and blue as the step v of v / 2048, little-endian 16-bit, in
// row-major order. It prints the count, the FNV-1a of those bytes and their SHA-256.
import Foundation
import CoreVideo
import IOSurface
import Metal
import CryptoKit

func fail(_ message: String) -> Never { FileHandle.standardError.write("error: \(message)\n".data(using: .utf8)!); exit(2) }

let W = 40, H = 24, ROW = 64, CHROMA_AT = 64 * 24, CW = 20, CH = 12, LENGTH = 4096

// The lane's generator: an LCG whose high bits are the bytes.
struct Random {
    var state: UInt64
    mutating func next() -> UInt8 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return UInt8(truncatingIfNeeded: state >> 33)
    }
}

func surfaceBytes(_ seed: UInt64) -> [UInt8] {
    var random = Random(state: seed)
    var bytes = [UInt8](repeating: 0xa5, count: LENGTH)
    for y in 0..<H { for x in 0..<W { bytes[y * ROW + x] = random.next() } }
    for y in 0..<CH { for x in 0..<(2 * CW) { bytes[CHROMA_AT + y * ROW + x] = random.next() } }
    return bytes
}

// What CoreVideo lays out for a 40 x 24 '420v' buffer, for the record.
func coreVideoPlanes() -> String {
    var buffer: CVPixelBuffer?
    let attributes: [String: Any] = [kCVPixelBufferIOSurfacePropertiesKey as String: [:] as CFDictionary]
    let status = CVPixelBufferCreate(nil, W, H, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                     attributes as CFDictionary, &buffer)
    guard status == kCVReturnSuccess, let buffer, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
    else { return "none (\(status))" }
    var out: [String] = []
    for p in 0..<IOSurfaceGetPlaneCount(surface) {
        out.append("plane \(p): \(IOSurfaceGetWidthOfPlane(surface, p))x\(IOSurfaceGetHeightOfPlane(surface, p))"
            + " bpr=\(IOSurfaceGetBytesPerRowOfPlane(surface, p)) bpe=\(IOSurfaceGetBytesPerElementOfPlane(surface, p))"
            + " ew=\(IOSurfaceGetElementWidthOfPlane(surface, p)) eh=\(IOSurfaceGetElementHeightOfPlane(surface, p))")
    }
    return out.joined(separator: "; ")
}

func plane(_ w: Int, _ h: Int, _ offset: Int, _ bytesPerElement: Int) -> [String: Any] {
    [
        kIOSurfacePlaneWidth as String: w,
        kIOSurfacePlaneHeight as String: h,
        kIOSurfacePlaneBytesPerRow as String: ROW,
        kIOSurfacePlaneOffset as String: offset,
        kIOSurfacePlaneSize as String: ROW * h,
        kIOSurfacePlaneBytesPerElement as String: bytesPerElement,
        kIOSurfacePlaneElementWidth as String: 1,
        kIOSurfacePlaneElementHeight as String: 1,
    ]
}

let properties: [String: Any] = [
    kIOSurfaceWidth as String: W,
    kIOSurfaceHeight as String: H,
    kIOSurfaceBytesPerElement as String: 1,
    kIOSurfaceAllocSize as String: LENGTH,
    kIOSurfacePixelFormat as String: Int(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    kIOSurfacePlaneInfo as String: [plane(W, H, 0, 1), plane(CW, CH, CHROMA_AT, 2)],
]

guard let surface = IOSurfaceCreate(properties as CFDictionary) else { fail("no IOSurface") }
let bytes = surfaceBytes(5)
guard IOSurfaceLock(surface, [], nil) == kIOReturnSuccess else { fail("lock") }
memcpy(IOSurfaceGetBaseAddress(surface), bytes, bytes.count)
IOSurfaceUnlock(surface, [], nil)

guard let device = MTLCreateSystemDefaultDevice() else { fail("no Metal device") }
let descriptor = MTLTextureDescriptor()
descriptor.textureType = .type2D
guard let ycbcr = MTLPixelFormat(rawValue: 500) else { fail("no format 500") }
descriptor.pixelFormat = ycbcr
descriptor.width = W
descriptor.height = H
descriptor.usage = .shaderRead
descriptor.storageMode = .shared
guard let texture = device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
else { fail("no texture over the surface") }

let source = """
#include <metal_stdlib>
using namespace metal;
kernel void read_texels(texture2d<float> t [[texture(0)]],
                        device float4 *out [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
  out[gid.y * \(W) + gid.x] = t.read(gid);
}
"""
let library: MTLLibrary
do { library = try device.makeLibrary(source: source, options: nil) } catch { fail("library: \(error)") }
guard let function = library.makeFunction(name: "read_texels") else { fail("no function") }
let state: MTLComputePipelineState
do { state = try device.makeComputePipelineState(function: function) } catch { fail("pipeline: \(error)") }

guard let out = device.makeBuffer(length: 16 * W * H, options: .storageModeShared),
      let queue = device.makeCommandQueue(),
      let commands = queue.makeCommandBuffer(),
      let encoder = commands.makeComputeCommandEncoder()
else { fail("no command buffer") }
encoder.setComputePipelineState(state)
encoder.setTexture(texture, index: 0)
encoder.setBuffer(out, offset: 0, index: 0)
encoder.dispatchThreads(MTLSize(width: W, height: H, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
encoder.endEncoding()
commands.commit()
commands.waitUntilCompleted()
if let error = commands.error { fail("commands: \(error)") }

let texels = out.contents().bindMemory(to: Float.self, capacity: 4 * W * H)
var packed = Data()
var offGrid = 0
for i in 0..<(W * H) {
    let alpha = texels[4 * i + 3]
    if alpha != 1.0 { fail("texel \(i): alpha \(alpha)") }
    for c in 0..<3 {
        let v = Double(texels[4 * i + c]) * 2048.0
        if v != v.rounded() || v < 0 || v > 2048 { offGrid += 1 }
        let step = UInt16(max(0, min(2048, v.rounded())))
        packed.append(UInt8(step & 0xff))
        packed.append(UInt8(step >> 8))
    }
}
var fnv: UInt32 = 2_166_136_261
for b in packed { fnv = (fnv ^ UInt32(b)) &* 16_777_619 }
let sha = SHA256.hash(data: packed).map { String(format: "%02x", $0) }.joined()
print("device \(device.name)")
print("core-video \(coreVideoPlanes())")
print("texels \(W * H) bytes \(packed.count) off-grid \(offGrid)")
print("fnv1a32 0x\(String(format: "%08x", fnv))")
print("sha256 \(sha)")
if CommandLine.arguments.count > 1 {
    try packed.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
    print("wrote \(CommandLine.arguments[1])")
}
