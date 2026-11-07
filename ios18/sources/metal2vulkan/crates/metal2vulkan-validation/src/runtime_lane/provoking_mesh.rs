//! Device tests of Apple's provoking vertex mode (`setProvokingVertexMode:`) on mesh draws, on the
//! device the cfg selects, under the validation layer: as the device is made, with
//! `VK_EXT_mesh_shader` withheld (the mesh function's records drawn by a synthesized vertex
//! stage), and each with the device's last provoking vertex (`VK_EXT_provoking_vertex`'s
//! `provokingVertexLast`) withheld. The owned fixture `rt6_provoking_mesh` draws four triangles
//! (`pm_triangles`) or six lines (`pm_lines`) of twelve vertices whose indices name them in
//! shuffled orders, each vertex's index its flat and smooth value; its fragment functions write
//! the weights, the smooth value and the flat index (`pm_both`), or the flat index alone
//! (`pm_flat`) or the weights alone (`pm_weights`).
//!
//! Metal's answers (a probe on the Apple M3 Max, macOS 27.0.1, of the same mesh functions): at
//! mode 1 a primitive's flat inputs read its last index, and its weights and the pixels it covers
//! are mode 0's, bit for bit, for triangles and lines alike.
//!
//! So each mesh and fragment function draws once at mode 0 and once at mode 1, and every pixel
//! both draws cover is checked: its flat index at mode 0 is some primitive's first, and at mode 1
//! that primitive's last; its smooth value is the weights' sum over the primitive's indices in
//! their order, at both modes; and its weights are mode 0's, bit for bit, but where the runtime
//! announces otherwise. Through the device's last provoking vertex every draw is Metal's exactly.
//! An emulated mesh with the last vertex withheld draws each primitive from its last index first
//! (`BindState::mesh_last_first`): a triangle's weights are taken back to Metal's order within
//! `2^-15` where its fragment function reads flat inputs too (announced,
//! `provoking-vertex-unmatched`), and a line of a pipeline that reads flat inputs runs the other
//! way (announced, `provoking-vertex-lines`), its weights and the pixels it covers unchecked.
//! With the extension's mesh pipeline and no last vertex, mode 1 draws mode 0's pixels, announced
//! (`provoking-vertex-unmatched`) where the pipeline reads flat inputs; one that reads the weights
//! alone draws Metal's, unannounced, on every path.

use super::provoking::PROVOKING;
use super::{layer_reported_nothing_at_all, validated_device_with, Messages, ScratchCache, WAIT};
use crate::air_library::AirLibrary;
use crate::gpu::TEST_DEVICE;
use metal2vulkan::device::{VkExtension, VkFeature};
use metal2vulkan::error::Feature;
use metal2vulkan::metal::{PixelFormat, TextureUsage};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, MeshPipelineDescriptor, StageSource};
use metal2vulkan_devtools::{engine_dir, llvm_assemble};
use metal2vulkan_runtime::{
    ClearColor, CommandBufferStatus, CommandQueue, Degradation, Device, Error, Library, LoadAction,
    PipelineOptions, Region, RenderPassDescriptor, RenderPipelineState, ResourceOptions, Size,
    StoreAction, TextureDescriptor,
};
use std::collections::BTreeSet;

/// The target's side, in pixels: the probe's.
const SIDE: u64 = 64;

/// The attachments' formats: the weights, the smooth value, the flat index.
const FORMATS: [PixelFormat; 3] = [
    PixelFormat::RGBA32Float,
    PixelFormat::RGBA32Float,
    PixelFormat::RGBA32Uint,
];

/// `pm_triangles`' primitives, their indices in order.
const TRIANGLES: [&[u32]; 4] = [&[0, 1, 2], &[5, 3, 4], &[8, 6, 7], &[10, 11, 9]];

/// `pm_lines`' primitives, their indices in order.
const LINES: [&[u32]; 6] = [&[0, 1], &[3, 2], &[4, 7], &[9, 5], &[6, 10], &[11, 8]];

/// A fragment function of the fixture, and what it reads.
#[derive(Clone, Copy, Debug)]
struct Fragment {
    name: &'static str,
    flat: bool,
    weights: bool,
}

const FRAGMENTS: [Fragment; 3] = [
    Fragment {
        name: "pm_both",
        flat: true,
        weights: true,
    },
    Fragment {
        name: "pm_flat",
        flat: true,
        weights: false,
    },
    Fragment {
        name: "pm_weights",
        flat: false,
        weights: true,
    },
];

/// Every attachment's texels, each texel's four words.
type Texels = Vec<Vec<[u32; 4]>>;

/// How the device draws a mesh at mode 1.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Path {
    /// Through its last provoking vertex: Metal's.
    Native,
    /// An emulated mesh drawn from each primitive's last index first.
    LastCorners,
    /// The extension's mesh pipeline without the last vertex: mode 0's.
    AsModeZero,
}

/// The lane's device, queue and the fixture's library.
struct Lane {
    device: Device,
    queue: CommandQueue,
    library: Library,
}

impl Lane {
    fn new(device: Device) -> Lane {
        let bitcode = llvm_assemble(&engine_dir().join("rt6_provoking_mesh.ll")).unwrap();
        Lane {
            queue: device.new_command_queue().unwrap(),
            library: device.new_library_with_air(&bitcode).unwrap(),
            device,
        }
    }

    /// The state of mesh function `mesh` with `fragment`'s, or its refusal.
    fn pipeline(&self, mesh: &str, fragment: Fragment) -> Result<RenderPipelineState, Error> {
        let function = |name| {
            StageSource::Function(
                self.library
                    .new_function_with_name(name)
                    .unwrap()
                    .function_descriptor(),
            )
        };
        let mut descriptor = MeshPipelineDescriptor::new(function(mesh));
        descriptor.fragment = Some(function(fragment.name));
        for (i, format) in FORMATS.iter().enumerate() {
            descriptor.color_attachments[i] = Some(ColorAttachmentDescriptor::new(*format));
        }
        self.device
            .new_mesh_render_pipeline_state(&descriptor, &PipelineOptions::default())
    }

    /// One mesh threadgroup of twelve threads drawn by `pipeline` at provoking vertex mode `mode`
    /// over cleared targets: every attachment's texels.
    fn draw(&self, pipeline: &RenderPipelineState, mode: u64) -> Texels {
        let targets: Vec<_> = FORMATS
            .iter()
            .map(|format| {
                let mut descriptor = TextureDescriptor::texture_2d(*format, SIDE, SIDE, false);
                descriptor.usage = TextureUsage::RENDER_TARGET;
                descriptor.resource_options = ResourceOptions::shared();
                self.device.new_texture(&descriptor).unwrap()
            })
            .collect();
        let mut pass = RenderPassDescriptor::new();
        for (i, target) in targets.iter().enumerate() {
            let color = &mut pass.color_attachments[i];
            color.texture = Some(target.clone());
            color.load_action = LoadAction::Clear;
            color.clear_color = ClearColor::new(0.0, 0.0, 0.0, 0.0);
            color.store_action = StoreAction::Store;
        }
        let command_buffer = self.queue.command_buffer().unwrap();
        let mut encoder = command_buffer.render_command_encoder(&pass).unwrap();
        encoder.set_render_pipeline_state(pipeline).unwrap();
        encoder.set_provoking_vertex_mode(mode).unwrap();
        let one = Size::new(1, 1, 1);
        encoder
            .draw_mesh_threadgroups(one, one, Size::new(12, 1, 1))
            .unwrap();
        encoder.end_encoding().unwrap();
        command_buffer.commit().unwrap();
        command_buffer.wait_until_completed_for(WAIT).unwrap();
        assert_eq!(
            command_buffer.status(),
            CommandBufferStatus::Completed,
            "{:?}",
            command_buffer.error()
        );
        targets
            .iter()
            .map(|target| {
                let mut out = vec![0u8; (SIDE * SIDE * 16) as usize];
                target
                    .get_bytes(
                        &mut out,
                        SIDE * 16,
                        SIDE * SIDE * 16,
                        Region::new_2d(0, 0, SIDE, SIDE),
                        0,
                        0,
                    )
                    .unwrap();
                out.as_chunks::<16>()
                    .0
                    .iter()
                    .map(|t| {
                        let (words, _) = t.as_chunks::<4>();
                        std::array::from_fn(|i| u32::from_le_bytes(words[i]))
                    })
                    .collect()
            })
            .collect()
    }
}

/// Whether `weights` and `smooth` are those of `primitive`'s indices in order: the weights' sum
/// over them is the smooth value.
fn weighs(weights: [f32; 4], smooth: f32, primitive: &[u32]) -> bool {
    let sum: f32 = primitive
        .iter()
        .zip(weights)
        .map(|(&v, w)| v as f32 * w)
        .sum();
    (sum - smooth).abs() <= 0.02
}

/// What one mesh and fragment function's two draws showed.
#[derive(Default, Debug)]
struct Seen {
    /// Pixels both draws cover.
    covered: usize,
    /// Pixels one draw covers and the other does not.
    moved: usize,
    /// Pixels whose flat index differs between the modes.
    flat_moved: usize,
}

/// Check `first` and `last`, the draws at modes 0 and 1 of mesh `primitives` under `fragment`,
/// as `path` draws them (the module docs), `what` naming them in failures.
fn check(
    what: &str,
    primitives: &[&[u32]],
    fragment: Fragment,
    path: Path,
    (first, last): (&Texels, &Texels),
) -> Seen {
    let lines = primitives[0].len() == 2;
    // A line drawn from its last vertex to its first, announced: its weights and coverage are
    // left unchecked.
    let reversed = path == Path::LastCorners && lines && fragment.flat;
    // A triangle drawn from its last vertex, its weights taken back to Metal's order but for
    // rounding, announced.
    let rotated = path == Path::LastCorners && !lines && fragment.flat && fragment.weights;
    let mut seen = Seen::default();
    for i in 0..first[0].len() {
        let (a, b) = (first[2][i], last[2][i]);
        if a[3] != b[3] {
            seen.moved += 1;
            continue;
        }
        if a[3] != 1 {
            continue;
        }
        seen.covered += 1;
        let weights = |t: &Texels| t[0][i].map(f32::from_bits);
        let smooth = |t: &Texels| f32::from_bits(t[1][i][0]);
        let primitive = match fragment.flat {
            true => {
                let p = primitives.iter().find(|p| p[0] == a[0]);
                let p = p.unwrap_or_else(|| {
                    panic!(
                        "{what}: pixel {i}'s flat index {} at mode 0 is no first",
                        a[0]
                    )
                });
                let want = match path {
                    Path::AsModeZero => p[0],
                    _ => *p.last().unwrap(),
                };
                assert_eq!(
                    b[0], want,
                    "{what}: pixel {i}'s flat index at mode 1 of {p:?}"
                );
                seen.flat_moved += usize::from(a[0] != b[0]);
                Some(*p)
            }
            false => None,
        };
        if !fragment.weights {
            continue;
        }
        let of = |t: &Texels| match primitive {
            Some(p) => weighs(weights(t), smooth(t), p),
            None => primitives.iter().any(|p| weighs(weights(t), smooth(t), p)),
        };
        assert!(
            of(first),
            "{what}: pixel {i}'s weights {:?} at mode 0 are not of {primitive:?}: {}",
            weights(first),
            smooth(first)
        );
        if reversed {
            continue;
        }
        assert!(
            of(last),
            "{what}: pixel {i}'s weights {:?} at mode 1 are not of {primitive:?}: {}",
            weights(last),
            smooth(last)
        );
        let (w0, w1) = (weights(first), weights(last));
        match rotated {
            true => assert!(
                w0.iter()
                    .zip(w1)
                    .all(|(x, y)| (x - y).abs() <= 1.0 / 32768.0),
                "{what}: pixel {i}'s weights at mode 1 {w1:?} are not mode 0's {w0:?}"
            ),
            false => assert_eq!(
                first[0][i], last[0][i],
                "{what}: pixel {i}'s weights at mode 1 {w1:?} are not mode 0's {w0:?}"
            ),
        }
    }
    if !reversed {
        assert_eq!(seen.moved, 0, "{what}: pixels covered at one mode only");
    }
    seen
}

/// Every mesh and fragment function at modes 0 and 1 on the cfg's device, with its last
/// provoking vertex withheld where `withheld` and `VK_EXT_mesh_shader` withheld where `emulated`:
/// the checks and the notices the module docs state.
fn run(withheld: bool, emulated: bool) {
    let cache = ScratchCache::new().unwrap();
    let messages = Messages::default();
    {
        let device = validated_device_with(TEST_DEVICE, &messages, cache.path(), |options| {
            if withheld {
                options
                    .withheld_features
                    .insert(VkFeature::ProvokingVertexLast);
            }
            // The extension's features go with it.
            if emulated {
                options
                    .withheld_extensions
                    .insert(VkExtension::ExtMeshShader);
                options
                    .withheld_features
                    .extend([VkFeature::TaskShader, VkFeature::MeshShader]);
            }
        })
        .unwrap();
        let enabled = device.compile_device().profile().enabled().clone();
        let native = enabled.features.contains(&VkFeature::ProvokingVertexLast);
        let meshes = enabled.extensions.contains(&VkExtension::ExtMeshShader)
            && enabled.features.contains(&VkFeature::MeshShader);
        let path = match (native, meshes) {
            (true, _) => Path::Native,
            (false, false) => Path::LastCorners,
            (false, true) => Path::AsModeZero,
        };
        let barycentric = enabled
            .features
            .contains(&VkFeature::FragmentShaderBarycentric);
        let name = device.name().to_owned();
        let lane = Lane::new(device);
        let mut announced = BTreeSet::new();
        let mut expected = BTreeSet::new();
        for (mesh, primitives) in [("pm_triangles", &TRIANGLES[..]), ("pm_lines", &LINES[..])] {
            let lines = primitives[0].len() == 2;
            for fragment in FRAGMENTS {
                let what = format!("{mesh} {} on {name} ({path:?})", fragment.name);
                // A mesh function's pipeline reads the weights through BaryCoordKHR alone.
                let pipeline = match lane.pipeline(mesh, fragment) {
                    Ok(pipeline) => pipeline,
                    Err(Error::Unsupported(refused))
                        if fragment.weights
                            && !barycentric
                            && refused.feature == Feature::Barycentrics =>
                    {
                        eprintln!("{what}: refused without fragmentShaderBarycentric: skipped");
                        continue;
                    }
                    Err(e) => panic!("{what}: {e}"),
                };
                let (first, last) = (lane.draw(&pipeline, 0), lane.draw(&pipeline, 1));
                let seen = check(&what, primitives, fragment, path, (&first, &last));
                eprintln!("{what}: {seen:?}");
                assert!(seen.covered > 100, "{what}: {seen:?}");
                if fragment.flat {
                    assert_eq!(seen.flat_moved > 0, path != Path::AsModeZero, "{what}");
                }
                if path == Path::AsModeZero {
                    assert!(first == last, "{what}: mode 1 is not mode 0's");
                }
                let kinds = messages.take_notices();
                let kinds = kinds.iter().filter_map(|m| m.degradation.as_ref());
                announced.extend(
                    kinds
                        .map(|notice| notice.degradation)
                        .filter(|kind| PROVOKING.contains(kind)),
                );
                let kind = match path {
                    Path::Native => None,
                    Path::LastCorners if lines && fragment.flat => {
                        Some(Degradation::ProvokingVertexLines)
                    }
                    Path::LastCorners if fragment.flat && fragment.weights => {
                        Some(Degradation::ProvokingVertexUnmatched)
                    }
                    Path::LastCorners => None,
                    Path::AsModeZero if fragment.flat => {
                        Some(Degradation::ProvokingVertexUnmatched)
                    }
                    Path::AsModeZero => None,
                };
                expected.extend(kind);
            }
        }
        assert_eq!(announced, expected, "{name} ({path:?})");
    }
    layer_reported_nothing_at_all(&messages);
}

/// Every mesh draw draws Metal's mode 1 on the device as it is made.
#[test]
fn mesh_draws_take_metal_s_last_vertex() {
    run(false, false);
}

/// Every mesh draw draws Metal's mode 1 through the emulation, `VK_EXT_mesh_shader` withheld.
#[test]
fn emulated_mesh_draws_take_metal_s_last_vertex() {
    run(false, true);
}

/// Every emulated mesh draw draws Metal's mode 1 from each primitive's last index first, the
/// device's last provoking vertex withheld.
#[test]
fn emulated_mesh_draws_take_metal_s_last_vertex_listed() {
    run(true, true);
}

/// A mesh draw through the extension without the last vertex draws mode 0's pixels at mode 1,
/// announced where its pipeline reads flat inputs (the emulation's, as the last test's, where the
/// device has no `VK_EXT_mesh_shader`).
#[test]
fn mesh_draws_without_the_last_vertex() {
    run(true, false);
}
