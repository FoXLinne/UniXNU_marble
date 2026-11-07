//! Metal's command predication on this host, the probe the runtime's answers come from: its
//! private compute-encoder calls (`encodeStartIf:offset:comparison:referenceValue:` and the rest)
//! driven through the Objective-C runtime, over our own synthetic MSL kernels. Each test runs the
//! streams the runtime lane asserts on Vulkan (`runtime_lane::predication`) and requires Metal to
//! give the answers recorded there, measured on an M3 Max under macOS 27.0.1 (Apple9). A Metal
//! that answers otherwise fails here, naming the streams.

use crate::gpu::compile;
use crate::gpu::wait::metal::commit_and_wait;
use crate::runtime_lane::predication::{
    cmp_rows, loop_cases, measured_streams, CMP_BRANCHES, PAST,
};
use core::ffi::c_void;
use core::ptr::NonNull;
use objc2::msg_send;
use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2_foundation::{NSRange, NSString};
use objc2_metal::{
    MTLBarrierScope, MTLBlitCommandEncoder, MTLBuffer, MTLClearColor, MTLCommandBuffer,
    MTLCommandEncoder, MTLCommandQueue, MTLComputeCommandEncoder, MTLComputePipelineState,
    MTLDevice, MTLDispatchType, MTLFence, MTLHeap, MTLHeapDescriptor, MTLHeapType, MTLLibrary,
    MTLLoadAction, MTLOrigin, MTLPixelFormat, MTLRenderPassDescriptor, MTLResourceOptions,
    MTLResourceUsage, MTLSize, MTLStorageMode, MTLStoreAction, MTLTextureDescriptor,
    MTLTextureUsage,
};

type Buffer = Retained<ProtocolObject<dyn MTLBuffer>>;
type Encoder = Retained<ProtocolObject<dyn MTLComputeCommandEncoder>>;
type CommandBuffer = Retained<ProtocolObject<dyn MTLCommandBuffer>>;

/// The kernels: `prd_mark` adds 1 to word `t` of `log`; `prd_put` writes `v` into word 0 of `w`.
const KERNELS: &str = "#include <metal_stdlib>
using namespace metal;
kernel void prd_mark(constant uint& t [[buffer(0)]], device uint* log [[buffer(1)]]) { log[t] += 1; }
kernel void prd_put(constant uint& v [[buffer(0)]], device uint* w [[buffer(1)]]) { w[0] = v; }
";

/// The log's words.
const LOG_WORDS: usize = 256;

/// What a stream runs with on Metal: the kernels' states, the log, the condition words (word 0 is
/// 1), indirect dispatch arguments (one threadgroup of one at 0; a grid of one thread in a group
/// of one at 16), and a fence.
struct Probe {
    queue: Retained<ProtocolObject<dyn MTLCommandQueue>>,
    mark: Retained<ProtocolObject<dyn MTLComputePipelineState>>,
    put: Retained<ProtocolObject<dyn MTLComputePipelineState>>,
    log: Buffer,
    word: Buffer,
    arguments: Buffer,
    fence: Retained<ProtocolObject<dyn MTLFence>>,
}

/// A stream's end on Metal: the count of each tag, what its Ends answered, and how its command
/// buffer ended (`Ok` where it completed; the bounded wait's error where not).
#[derive(Debug, PartialEq, Eq)]
struct Ran {
    counts: Vec<u32>,
    answers: String,
    settled: Result<(), String>,
}

impl Ran {
    /// The tags that ran, ascending, where each ran once.
    fn once(&self) -> Option<Vec<u32>> {
        if self.counts.iter().any(|&n| n > 1) {
            return None;
        }
        Some(
            (0..LOG_WORDS as u32)
                .filter(|&t| self.counts[t as usize] == 1)
                .collect(),
        )
    }
}

fn shared(device: &ProtocolObject<dyn MTLDevice>, words: &[u32]) -> Buffer {
    let buffer = device
        .newBufferWithLength_options(
            4 * words.len().max(1),
            MTLResourceOptions::StorageModeShared,
        )
        .expect("a Shared buffer");
    write(&buffer, words);
    buffer
}

/// Write `words` from word 0 of a Shared `buffer`.
fn write(buffer: &ProtocolObject<dyn MTLBuffer>, words: &[u32]) {
    let contents = buffer.contents().cast::<u32>();
    // SAFETY: the Shared buffer holds at least `words.len()` words, and no command buffer runs
    // while the CPU writes them.
    unsafe { std::ptr::copy_nonoverlapping(words.as_ptr(), contents.as_ptr(), words.len()) };
}

fn one() -> MTLSize {
    MTLSize {
        width: 1,
        height: 1,
        depth: 1,
    }
}

impl Probe {
    fn new() -> Probe {
        let device = super::system_device().expect("a Metal device");
        let library = compile::bounded("predication probe library compilation", || {
            device.newLibraryWithSource_options_error(&NSString::from_str(KERNELS), None)
        })
        .expect("the kernels compile");
        let state = |name: &str| {
            let function = library
                .newFunctionWithName(&NSString::from_str(name))
                .expect("the kernel");
            compile::bounded("predication probe pipeline creation", || {
                device.newComputePipelineStateWithFunction_error(&function)
            })
            .expect("its state")
        };
        Probe {
            queue: device.newCommandQueue().expect("a queue"),
            mark: state("prd_mark"),
            put: state("prd_put"),
            log: shared(&device, &[0; LOG_WORDS]),
            word: shared(&device, &[1, 0, 0, 0]),
            arguments: shared(&device, &[1, 1, 1, 0, 1, 1, 1, 1, 1, 1]),
            fence: device.newFence().expect("a fence"),
        }
    }

    /// Bind `prd_mark` to mark `tag`.
    fn bind(&self, encoder: &Encoder, tag: u32) {
        encoder.setComputePipelineState(&self.mark);
        self.bytes(encoder, tag);
        // SAFETY: the log outlives the command buffer, which the probe waits for.
        unsafe { encoder.setBuffer_offset_atIndex(Some(&self.log), 0, 1) };
    }

    fn bytes(&self, encoder: &Encoder, value: u32) {
        let bytes = NonNull::from(&value).cast::<c_void>();
        // SAFETY: four bytes of a live `u32`, copied by the call.
        unsafe { encoder.setBytes_length_atIndex(bytes, 4, 0) };
    }

    /// Run `script` in one command buffer, in the runtime lane's notation (`runtime_lane::
    /// predication::run`), and these for loops that count: `w<n>[@k]` starts a While on log word
    /// `k` (0 where none) `!= n`, `z<n>[@k]` ends a DoWhile on it `< n`, `i<n>[@k]` starts an If
    /// on it `== n`, `Q<k>` writes 0 into log word `k`, `y<n>` sets the bytes to `n`, `o<n>` sets
    /// buffer 1's offset to word `n`.
    fn run(&self, script: &str) -> Ran {
        write(&self.log, &[0; LOG_WORDS]);
        write(&self.word, &[1, 0, 0, 0]);
        let command_buffer: CommandBuffer = self.queue.commandBuffer().expect("a command buffer");
        let mut open: Option<Encoder> = None;
        let mut answers = String::new();
        for token in script.split_whitespace() {
            match token {
                "|" | "%" => {
                    if let Some(encoder) = open.take() {
                        encoder.endEncoding();
                    }
                    let dispatch_type = match token {
                        "%" => MTLDispatchType::Concurrent,
                        _ => MTLDispatchType::Serial,
                    };
                    open = command_buffer.computeCommandEncoderWithDispatchType(dispatch_type);
                    continue;
                }
                "B" => {
                    if let Some(encoder) = open.take() {
                        encoder.endEncoding();
                    }
                    let blit = command_buffer.blitCommandEncoder().expect("a blit encoder");
                    blit.fillBuffer_range_value(&self.word, NSRange::new(8, 4), 1);
                    blit.endEncoding();
                    continue;
                }
                _ => {}
            }
            let encoder = open.get_or_insert_with(|| {
                command_buffer
                    .computeCommandEncoder()
                    .expect("a compute encoder")
            });
            if let Some(answer) = self.record(encoder, token) {
                answers.push(if answer { '1' } else { '0' });
            }
        }
        if let Some(encoder) = open {
            encoder.endEncoding();
        }
        let settled = commit_and_wait(&command_buffer, "Metal predication stream");
        let contents = self.log.contents().cast::<u32>();
        // SAFETY: the Shared log's words, read once the command buffer's work ended.
        let counts = unsafe { std::slice::from_raw_parts(contents.as_ptr(), LOG_WORDS) }.to_vec();
        Ran {
            counts,
            answers,
            settled,
        }
    }

    /// Encode `token` on `encoder`: an End's answer, if it is one.
    fn record(&self, encoder: &Encoder, token: &str) -> Option<bool> {
        let e = &**encoder;
        let word = Some(&*self.word);
        let none: Option<&ProtocolObject<dyn MTLBuffer>> = None;
        let number = |token: &str| -> (u32, usize) {
            let (n, k) = token[1..].split_once('@').unwrap_or((&token[1..], "0"));
            (n.parse().unwrap(), k.parse().unwrap())
        };
        // SAFETY: each private selector takes these arguments (`v@:@QQI`, `v@:`, `B@:` and
        // `B@:@QQI`), on a live compute encoder, its buffers alive until the command buffer
        // completed.
        unsafe {
            match token {
                "I1" | "I0" | "In" | "W1" | "W0" | "Wn" => {
                    let buffer = if token.ends_with('n') { none } else { word };
                    let reference: u32 = u32::from(token.ends_with('1'));
                    match token.starts_with('I') {
                        true => {
                            let _: () = msg_send![e, encodeStartIf: buffer, offset: 0usize, comparison: 0usize, referenceValue: reference];
                        }
                        false => {
                            let _: () = msg_send![e, encodeStartWhile: buffer, offset: 0usize, comparison: 0usize, referenceValue: reference];
                        }
                    }
                    None
                }
                "L" => {
                    let _: () = msg_send![e, encodeStartElse];
                    None
                }
                "D" => {
                    let _: () = msg_send![e, encodeStartDoWhile];
                    None
                }
                "E" => Some(msg_send![e, encodeEndIf]),
                "V" => Some(msg_send![e, encodeEndWhile]),
                "Z0" | "Zn" | "Zp" => {
                    let buffer = if token == "Zn" { none } else { word };
                    let offset = if token == "Zp" { PAST as usize } else { 0 };
                    Some(
                        msg_send![e, encodeEndDoWhile: buffer, offset: offset, comparison: 0usize, referenceValue: 0u32],
                    )
                }
                "U" => {
                    let usage = MTLResourceUsage::Read | MTLResourceUsage::Write;
                    encoder.useResource_usage(ProtocolObject::from_ref(&*self.log), usage);
                    None
                }
                "Fu" => {
                    encoder.updateFence(&self.fence);
                    None
                }
                "Fw" => {
                    encoder.waitForFence(&self.fence);
                    None
                }
                "M" => {
                    encoder.memoryBarrierWithScope(MTLBarrierScope::Buffers);
                    None
                }
                "t" => {
                    encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
                    None
                }
                _ => {
                    let (n, k) = number(token);
                    let log = Some(&*self.log);
                    let at = 4 * k;
                    match &token[..1] {
                        "T" => {
                            self.bind(encoder, n);
                            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
                        }
                        "G" => {
                            self.bind(encoder, n);
                            encoder.dispatchThreadgroupsWithIndirectBuffer_indirectBufferOffset_threadsPerThreadgroup(&self.arguments, 0, one());
                        }
                        "H" => {
                            self.bind(encoder, n);
                            let arguments = &*self.arguments;
                            let _: () = msg_send![e, dispatchThreadsWithIndirectBuffer: arguments, indirectBufferOffset: 16usize];
                        }
                        "b" => self.bind(encoder, n),
                        "y" => self.bytes(encoder, n),
                        "o" => encoder.setBufferOffset_atIndex(4 * n as usize, 1),
                        "Q" => {
                            encoder.setComputePipelineState(&self.put);
                            self.bytes(encoder, 0);
                            encoder.setBuffer_offset_atIndex(Some(&self.log), 4 * n as usize, 1);
                            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
                        }
                        "w" => {
                            let _: () = msg_send![e, encodeStartWhile: log, offset: at, comparison: 1usize, referenceValue: n];
                        }
                        "i" => {
                            let _: () = msg_send![e, encodeStartIf: log, offset: at, comparison: 0usize, referenceValue: n];
                        }
                        "z" => {
                            return Some(
                                msg_send![e, encodeEndDoWhile: log, offset: at, comparison: 4usize, referenceValue: n],
                            );
                        }
                        _ => panic!("no token {token}"),
                    }
                    None
                }
            }
        }
    }
}

/// Every stream the runtime lane asserts runs on Metal as recorded: the tags that ran, each once,
/// what its Ends answered, and a completed command buffer.
#[test]
fn metal_runs_the_measured_streams() {
    let probe = Probe::new();
    let streams = measured_streams();
    let mut wrong = Vec::new();
    for (name, script, ran, answers) in &streams {
        let got = probe.run(script);
        if got.once().as_ref() != Some(ran) || got.answers != *answers || got.settled.is_err() {
            wrong.push(format!(
                "{name} `{script}`: {got:?}, recorded {ran:?} {answers:?}"
            ));
        }
    }
    assert!(
        wrong.is_empty(),
        "{} of {}:\n{}",
        wrong.len(),
        streams.len(),
        wrong.join("\n")
    );
}

/// Metal's comparison codes and word offsets: each of the lane's `cmp` rows runs the branch
/// recorded in `CMP_BRANCHES`.
#[test]
fn metal_compares_as_recorded() {
    let probe = Probe::new();
    let rows = cmp_rows();
    assert_eq!(rows.len(), CMP_BRANCHES.len());
    let words = shared(
        &super::system_device().unwrap(),
        &[0x8000_0007, u32::MAX, 5, 0],
    );
    let command_buffer: CommandBuffer = probe.queue.commandBuffer().unwrap();
    let encoder = command_buffer.computeCommandEncoder().unwrap();
    write(&probe.log, &[0; LOG_WORDS]);
    for (row, &(code, offset, reference)) in rows.iter().enumerate() {
        let e = &*encoder;
        let words = Some(&*words);
        let (offset, code) = (offset as usize, code as usize);
        // SAFETY: as `Probe::record`'s.
        unsafe {
            let _: () = msg_send![e, encodeStartIf: words, offset: offset, comparison: code, referenceValue: reference];
            probe.bind(&encoder, 2 * row as u32);
            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
            let _: () = msg_send![e, encodeStartElse];
            probe.bind(&encoder, 2 * row as u32 + 1);
            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
            let end: bool = msg_send![e, encodeEndIf];
            assert!(end, "row {row}");
        }
    }
    encoder.endEncoding();
    commit_and_wait(&command_buffer, "Metal predication comparisons").unwrap();
    let contents = probe.log.contents().cast::<u32>();
    // SAFETY: the Shared log's words, read after the command buffer completed.
    let log = unsafe { std::slice::from_raw_parts(contents.as_ptr(), LOG_WORDS) };
    let branches: String = (0..rows.len())
        .map(|row| match (log[2 * row], log[2 * row + 1]) {
            (1, 0) => 'T',
            (0, 1) => 'E',
            _ => '?',
        })
        .collect();
    assert_eq!(branches, CMP_BRANCHES);
}

/// Metal's loops, as the runtime lane's loop tests expect them ([`loop_cases`]): the passes of
/// Whiles and DoWhiles of 0 to 64, a While in a concurrent encoder, three nested loops, each
/// pass's bindings, an If in a DoWhile; loops spanning a unit's encoders (serial, concurrent,
/// three, around an If, inside a spanning If, with an encoder's own bindings) and a serial
/// encoder's While its concurrent encoder ends, which kills it; six nested loops; loops of 65
/// to 100000 passes, past the copies the runtime lays down, which Metal runs on; and loops after
/// an unbalanced End that drops its unit's dispatches before it.
#[test]
fn metal_runs_loops_as_the_lane_expects() {
    let probe = Probe::new();
    for (script, want, answers) in loop_cases() {
        let got = probe.run(&script);
        let mut counts = vec![0; LOG_WORDS];
        for (tag, n) in want {
            counts[tag] = n;
        }
        assert_eq!(got.settled, Ok(()), "{script}");
        assert_eq!(got.counts, counts, "{script}");
        assert_eq!(got.answers, answers, "{script}");
    }
}

/// A condition word past its buffer's end, on Metal (the runtime lane's
/// `a_word_past_its_buffer_reads_as_metal_reads_it`, the same records): zero at offsets 16 and
/// 252 of a lone buffer of 16 bytes, the next buffer's word past a buffer placed at 0 of a
/// placement heap, zero past the heap's end and at offset 2^32 of the lone buffer, and a live
/// DoWhile's End far past its buffer reading zero, so `!= 0` fails and its body runs once.
#[test]
fn metal_reads_a_word_past_its_buffer_as_the_lane_expects() {
    let probe = Probe::new();
    write(&probe.log, &[0; LOG_WORDS]);
    write(&probe.word, &[1, 0, 0, 0]);
    let device = probe.queue.device();
    let heap_size = 1usize << 20;
    let descriptor = MTLHeapDescriptor::new();
    descriptor.setType(MTLHeapType::Placement);
    descriptor.setStorageMode(MTLStorageMode::Shared);
    descriptor.setSize(heap_size);
    let heap = device.newHeapWithDescriptor(&descriptor).expect("a heap");
    let options = MTLResourceOptions::StorageModeShared;
    // SAFETY: 256-byte buffers at 256-aligned offsets inside the heap.
    let (first, next) = unsafe {
        (
            heap.newBufferWithLength_options_offset(256, options, 0),
            heap.newBufferWithLength_options_offset(256, options, 65536),
        )
    };
    let (first, next) = (first.expect("a heap buffer"), next.expect("a heap buffer"));
    write(&next, &[0xb0b0_b0b0]);
    let command_buffer: CommandBuffer = probe.queue.commandBuffer().expect("a command buffer");
    let encoder = command_buffer
        .computeCommandEncoder()
        .expect("a compute encoder");
    let e = &*encoder;
    let mut tag = 0;
    let mut holds = |buffer: &ProtocolObject<dyn MTLBuffer>, offset: usize, word: u32| {
        // SAFETY: the private selectors take these arguments (`v@:@QQI`, `v@:`, `B@:`), on a
        // live compute encoder; the buffers outlive the command buffer, which the test waits for.
        unsafe {
            let _: () = msg_send![e, encodeStartIf: buffer, offset: offset, comparison: 0usize, referenceValue: word];
            probe.bind(&encoder, tag);
            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
            let _: () = msg_send![e, encodeStartElse];
            probe.bind(&encoder, tag + 1);
            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
            let end: bool = msg_send![e, encodeEndIf];
            assert!(end);
        }
        tag += 2;
    };
    holds(&probe.word, 16, 0);
    holds(&probe.word, 252, 0);
    holds(&first, 65536, 0xb0b0_b0b0);
    holds(&first, heap_size, 0);
    holds(&probe.word, 1 << 32, 0);
    // SAFETY: as above (`v@:` and `B@:@QQI`).
    unsafe {
        let _: () = msg_send![e, encodeStartDoWhile];
        probe.bind(&encoder, 20);
        encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
        let word = &*probe.word;
        let end: bool = msg_send![e, encodeEndDoWhile: word, offset: PAST as usize, comparison: 1usize, referenceValue: 0u32];
        assert!(end);
    }
    encoder.endEncoding();
    commit_and_wait(
        &command_buffer,
        "Metal predication words past their buffers",
    )
    .expect("the command buffer completes");
    let contents = probe.log.contents().cast::<u32>();
    // SAFETY: the Shared log's words, read once the command buffer's work ended.
    let counts = unsafe { std::slice::from_raw_parts(contents.as_ptr(), LOG_WORDS) };
    let ran: Vec<usize> = (0..LOG_WORDS).filter(|&t| counts[t] == 1).collect();
    assert_eq!(ran, [0, 2, 4, 6, 8, 20], "{:?}", &counts[..24]);
}

/// A block open where its unit ends, on Metal (the runtime lane's
/// `a_block_open_where_its_unit_ends_fails_where_metals_recovery_does`): where the unit's commands
/// run there (a held If, a held While, which ran once, a DoWhile, a failed If's Else) and in a
/// killed block, the command buffer completes with every mark recorded. Where they skip there,
/// Metal's GPU recovery fails the command buffer (`MTLCommandBufferErrorInternal`, `PageFault` in
/// some runs), which the bounded wait records as a GPU fault: those cases are not run here.
#[test]
fn metal_runs_a_block_left_open_where_its_commands_run() {
    let probe = Probe::new();
    let harmless: [(&str, &[u32]); 6] = [
        ("T5 B I1 T1 B T3", &[1, 3, 5]),
        ("T5 B W1 T1 B T3", &[1, 3, 5]),
        ("T5 B D T1 B T3", &[1, 3, 5]),
        ("T5 B I0 T1 L T2 B T3", &[2, 3, 5]),
        ("T5 B I1 V T1 B T3", &[3, 5]),
        ("T9 I1 T1 % E T3", &[1, 9]),
    ];
    for (script, ran) in harmless {
        let got = probe.run(script);
        assert_eq!(got.settled, Ok(()), "{script}");
        assert_eq!(got.once().as_deref(), Some(ran), "{script}");
    }
}

/// A loop past the copies the runtime lays down, after a render pass that clears a texture and
/// blits in its command buffer, on Metal (the runtime lane's
/// `a_loop_run_again_repeats_nothing_before_it`, the same commands): the pass clears the texture
/// red, a blit copies green into it, another 5 into log word 3, and `while (log[3] != 105)
/// { mark 3; mark 4 }` runs 100 passes; the texture reads green.
#[test]
fn metal_runs_a_loop_after_a_cleared_pass_and_blits_once() {
    let probe = Probe::new();
    write(&probe.log, &[0; LOG_WORDS]);
    let device = probe.queue.device();
    let descriptor = MTLTextureDescriptor::new();
    descriptor.setPixelFormat(MTLPixelFormat::RGBA8Unorm);
    // SAFETY: a 1 x 1 texture.
    unsafe {
        descriptor.setWidth(1);
        descriptor.setHeight(1);
    }
    descriptor.setStorageMode(MTLStorageMode::Shared);
    descriptor.setUsage(MTLTextureUsage::RenderTarget | MTLTextureUsage::ShaderRead);
    let texture = device
        .newTextureWithDescriptor(&descriptor)
        .expect("a texture");
    let green = shared(&device, &[u32::from_le_bytes([0, 255, 0, 255])]);
    let five = shared(&device, &[5]);
    let out = shared(&device, &[0]);
    let command_buffer: CommandBuffer = probe.queue.commandBuffer().expect("a command buffer");
    let pass = MTLRenderPassDescriptor::renderPassDescriptor();
    // SAFETY: attachment 0 of the pass's array.
    let color = unsafe { pass.colorAttachments().objectAtIndexedSubscript(0) };
    color.setTexture(Some(&texture));
    color.setLoadAction(MTLLoadAction::Clear);
    color.setStoreAction(MTLStoreAction::Store);
    color.setClearColor(MTLClearColor {
        red: 1.0,
        green: 0.0,
        blue: 0.0,
        alpha: 1.0,
    });
    command_buffer
        .renderCommandEncoderWithDescriptor(&pass)
        .expect("a render encoder")
        .endEncoding();
    let origin = MTLOrigin { x: 0, y: 0, z: 0 };
    let blit = command_buffer.blitCommandEncoder().expect("a blit encoder");
    // SAFETY: a texel of the texture and words of Shared buffers that outlive the command buffer,
    // which the test waits for.
    unsafe {
        blit.copyFromBuffer_sourceOffset_sourceBytesPerRow_sourceBytesPerImage_sourceSize_toTexture_destinationSlice_destinationLevel_destinationOrigin(
            &green, 0, 4, 4, one(), &texture, 0, 0, origin,
        );
        blit.copyFromBuffer_sourceOffset_toBuffer_destinationOffset_size(
            &five, 0, &probe.log, 12, 4,
        );
    }
    blit.endEncoding();
    let encoder = command_buffer
        .computeCommandEncoder()
        .expect("a compute encoder");
    let e = &*encoder;
    let log = &*probe.log;
    // SAFETY: the private selectors take these arguments (`v@:@QQI` and `B@:`), on a live compute
    // encoder; the log outlives the command buffer.
    unsafe {
        let _: () = msg_send![e, encodeStartWhile: log, offset: 12usize, comparison: 1usize, referenceValue: 105u32];
        for tag in [3, 4] {
            probe.bind(&encoder, tag);
            encoder.dispatchThreads_threadsPerThreadgroup(one(), one());
        }
        let end: bool = msg_send![e, encodeEndWhile];
        assert!(end);
    }
    encoder.endEncoding();
    let blit = command_buffer.blitCommandEncoder().expect("a blit encoder");
    // SAFETY: as above.
    unsafe {
        blit.copyFromTexture_sourceSlice_sourceLevel_sourceOrigin_sourceSize_toBuffer_destinationOffset_destinationBytesPerRow_destinationBytesPerImage(
            &texture, 0, 0, origin, one(), &out, 0, 4, 4,
        );
    }
    blit.endEncoding();
    commit_and_wait(&command_buffer, "Metal loop after a cleared pass and blits")
        .expect("the command buffer completes");
    let contents = probe.log.contents().cast::<u32>();
    // SAFETY: the Shared log's words, read once the command buffer's work ended.
    let counts = unsafe { std::slice::from_raw_parts(contents.as_ptr(), LOG_WORDS) };
    assert_eq!(counts[3..5], [105, 100]);
    // SAFETY: the Shared buffer's word, read once the command buffer's work ended.
    let texel = unsafe { out.contents().cast::<u32>().read() };
    assert_eq!(texel.to_le_bytes(), [0, 255, 0, 255]);
}
