//! What a kernel's reflection says its arguments are: `MTLBinding` and the type tree under it.
//!
//! The suite compared one bit of a reflection before this family — `texture_accesses` reads a
//! render pipeline's texture slots and their access — and nothing else. The whole of
//! `MTLBufferBinding`, `MTLTextureBinding`, `MTLThreadgroupBinding`, `MTLStructType`,
//! `MTLStructMember`, `MTLPointerType`, `MTLArrayType` and `MTLTextureReferenceType` was compared
//! against Metal nowhere.
//!
//! For an argument buffer that tree **is** the layout contract: every member's `offset` and
//! `argumentIndex`, every array's `stride` and `argumentIndexStride`, every pointer's `alignment`
//! and `dataSize`. [`crate::cases::argument_encoders`] compares where an encoder puts each
//! argument; this compares what Metal says each argument is. Checking one without the other is
//! checking an answer without its question.
//!
//! # The binding kinds, measured
//!
//! `MTLBindingType` holds twelve values and the suite's eight hundred recordings held **four** of
//! them anywhere: `Buffer`, `ThreadgroupMemory`, `Texture` and `Sampler`. Of the other eight, four
//! are ray tracing's and one is Metal 4's tensor, all out of this project's scope; `ObjectPayload`
//! needs a mesh pipeline's reflection, which no case reads, and is a named gap. The two that are
//! reachable from a compute pipeline are here: a **visible-function table**, whose kernel takes one
//! as an argument, and an **imageblock**, whose kernel declares one explicitly.
//!
//! Five kernels, chosen so that between them the three subprotocols a binding can conform to are
//! all reached, and so that two binding types nothing had compared are:
//!
//! - **the argument-buffer kernel** (`px_samplers`' `px_sample_argument`): two plain device
//!   pointers, which answer a `bufferPointerType`, and one argument buffer, which answers a
//!   `bufferStructType` whose members are a texture reference and a sampler;
//! - **the texture-slot kernel** (`xr_samples`' `xr_sample_runtime`): a `[[texture(0)]]` and a
//!   `[[sampler(0)]]`, which is the only way to reach `MTLTextureBinding`'s own fields;
//! - **the threadgroup kernel** (`a27_ob1_threadgroup`): a `threadgroup uint *` argument, which is
//!   the only way to reach `MTLThreadgroupBinding`'s.
//!
//! # `MTLDataType`, swept
//!
//! `MTLDataType` is the vocabulary every reflected binding, struct member, array element and
//! pointee uses to say what it holds, so an implementation that mis-reports one of its 97 values
//! mis-describes a shader's interface to every consumer that reflects it. The five kernels above
//! reach sixteen of those values between them. Ten more cases sweep it, each over a committed
//! fixture written for the reflection lane rather than a copy of one
//! (`fixtures/public/reflection/`, reached by the `<directory>/<name>` form
//! [`crate::fixture`] documents):
//!
//! - **the six `encoder_types` kernels**, one struct per MSL scalar, vector and matrix type that
//!   `MTLDataType` names — 66 of them — each held alone behind a `device` pointer
//!   (`encoder_types_plain_*`) and again after a texture in an argument buffer
//!   (`encoder_types_tail_*`);
//! - **`argument_buffers`' `reflection_argument_buffers`**, whose argument buffer holds arrays of
//!   textures, samplers, pointers, inline constants and structs, a nested argument buffer, an
//!   indirect command buffer and a compute pipeline state;
//! - **`encoder_arrays`' `encoder_arrays`**, ten argument buffers each holding one two-element
//!   array of structs, which is where an array's `stride` and `argumentIndexStride` are Metal's
//!   own arithmetic rather than a size the source states;
//! - **`textures`' `reflection_textures`**, thirteen texture types across four component types,
//!   which is where `textureDataType` is derived from a template parameter;
//! - **`tables_in_arguments`' `reflection_tables_in_arguments`**, a visible function table and an
//!   array of two inside an argument buffer.
//!
//! ## What each swept value moves, and why none of it is vacuous
//!
//! A reflection case that read back the type its own source declared would compare nothing, 66
//! times over. None of these sites is that. For each swept member type Metal **derives**, and the
//! numbers below are the ones Metal answered on an Apple M3 Max under Metal 27.0.1, read off these
//! cases' own committed recordings:
//!
//! - `bufferDataSize` and `bufferAlignment` of the struct the pointer argument points at, from
//!   Metal's own alignment rules and nothing in the text. Over the 66 types they take **16
//!   distinct (size, alignment) pairs**: `float` 4 and 4, `float3` 16 and 16, `float2x3` 32 and
//!   16, `half2x2` 8 and **4**, `half3x2` 12 and 4, `bool` 1 and 1, `long4` 32 and 32.
//! - the **offset** Metal computes for that member where it follows a texture in an argument
//!   buffer, which the texture's 8 bytes and the member's own alignment decide together: 8 for
//!   `float`, `half`, `char`, `bool` and `long`, 16 for `float3`, `float4`, `int3` and `long2`,
//!   32 for `long3` and `long4`.
//! - the **struct size** that offset and member leave, which Metal rounds up to the struct's own
//!   alignment rather than reporting the C size: the struct holding a texture and a `float`
//!   occupies 12 bytes (the committed AIR says `dereferenceable(12)`) and Metal's reflection
//!   answers **16**; a texture and a `bool` is 9 and answers 16; a texture and a `half4x3` is 40
//!   and answers 40. That rounding is a rule an implementation can get wrong in either direction
//!   and no source line states it.
//! - an array's `stride`, `arrayLength` and `argumentIndexStride`, counted and multiplied by
//!   Metal: a pipeline's reflection gives an array of resources, and of resource-only structs,
//!   stride **0**, and gives an array whose element holds a constant its element's size.
//!
//! So a value whose alignment or size an implementation gets wrong moves a number the case did not
//! supply.
//!
//! The numbers alone do not separate every value, and saying which ones they do not is part of the
//! claim rather than a footnote to it. Size and alignment collapse the 66 into those 16 classes,
//! and the largest are wide: 13 types answer 4 and 4 (`MTLDataTypeFloat`, `MTLDataTypeInt`,
//! `MTLDataTypeUInt`, `MTLDataTypeHalf2`, `MTLDataTypeBFloat2`, `MTLDataTypeShort2`,
//! `MTLDataTypeUShort2`, `MTLDataTypeChar3`, `MTLDataTypeChar4`, `MTLDataTypeUChar3`,
//! `MTLDataTypeUChar4`, `MTLDataTypeBool3`, `MTLDataTypeBool4`), 13 more answer 8 and 8, 7 answer
//! 2 and 2, and 3 answer 1 and 1. **Within a class the separating site is the `dataType`
//! enumerator itself**, and nothing else in the tree distinguishes those values at all.
//!
//! Reading that enumerator back is **not** the case reading its own declaration. What a case
//! supplies is a fixture's AIR; the enumerator is each side's own mapping from an AIR type to
//! Metal's vocabulary, which the Metal leg takes from `MTLStructMember.dataType` and ours derives
//! from the metadata ABI (`metal2vulkan`'s `reflection::types`, off `air.arg_type_name`). Two
//! independent derivations of one answer is a comparison, and mapping `bfloat` to
//! `MTLDataTypeHalf`, or a signed type to its unsigned neighbour, is exactly the class of bug it
//! catches — one no size could see, because those pairs have the same size.
//!
//! ## The values this sweep does not reach
//!
//! - `MTLDataTypeNone`, which is what a getter answers for a type it has no name for, so no
//!   well-formed declaration produces it;
//! - the sixteen packed pixel types (`MTLDataTypeR8Unorm` through `MTLDataTypeRGB9E5Float`), which
//!   a struct can state only as an imageblock member, and whose reflection a compute pipeline does
//!   not answer — `function-bindings-imageblock` records what Metal does answer for a kernel's
//!   imageblock, and it holds no struct type. Reaching them needs a render pipeline's tile
//!   reflection, which `MTLArgument`'s deprecation puts out of this harness's reach (above);
//! - `MTLDataTypeIntersectionFunctionTable`, `MTLDataTypePrimitiveAccelerationStructure` and
//!   `MTLDataTypeInstanceAccelerationStructure`, ray tracing's, out of this project's scope;
//! - `MTLDataTypeTensor`, Metal 4's, likewise;
//! - `MTLDataTypeDepthStencilState`, which an argument buffer can hold only through MSL's
//!   `depth_stencil_state`, a render-encoder type no compute kernel takes.
//!
//! # Why this family is safe
//!
//! Each case makes one compute pipeline state and reads its reflection. Nothing is dispatched,
//! nothing is allocated for the device, and no command buffer is made, so no case here can fault
//! the device or trip its watchdog. The pipeline creation is the one-time translation cost the
//! project's ceilings already cover.
//!
//! # Exactness
//!
//! Every field is a number, an enumeration or a name, and the family declares no tolerance. An
//! enumeration is recorded as its Metal enumerator name beside its ordinal, so a difference reads
//! `MTLDataTypeFloat2` against `MTLDataTypeFloat4` rather than 4 against 6.

use crate::case::{Abort, Case, Failure, Input, MetalSource, Side};
use crate::fixture::{Fixture, METAL_TEXT, RUNTIME_TEXT};
use crate::function_bindings::bindings_node;
use crate::recording::ShaderRef;
use crate::transcript::{Digest, Node, Observed, Sites};

/// The site the reflection's answer sits at.
const BINDINGS: &str = "bindings";

/// The fixture holding one struct per MSL scalar, vector and matrix type `MTLDataType` names.
const DATA_TYPES: &str = "reflection/encoder_types";

/// A case over one kernel's reflected arguments.
pub struct FunctionBindingsCase {
    name: &'static str,
    /// The fixture and the entry point whose reflection the case reads.
    fixture: &'static str,
    entry: &'static str,
}

/// Every case of the family.
pub fn all() -> Vec<FunctionBindingsCase> {
    vec![
        // The argument-buffer kernel: `constant PixelArguments &a [[buffer(2)]]` holding a texture
        // and a sampler, beside two plain device pointers at 0 and 1.
        FunctionBindingsCase {
            name: "function-bindings-argument-buffer",
            fixture: "px_samplers",
            entry: "px_sample_argument",
        },
        // The texture-slot kernel: `[[texture(0)]]` and `[[sampler(0)]]`, which is where
        // `MTLTextureBinding`'s own fields are.
        FunctionBindingsCase {
            name: "function-bindings-texture-slots",
            fixture: "xr_samples",
            entry: "xr_sample_runtime",
        },
        // The threadgroup-memory kernel: `threadgroup uint *tg [[threadgroup(0)]]`, which is where
        // `MTLThreadgroupBinding`'s are.
        FunctionBindingsCase {
            name: "function-bindings-threadgroup-memory",
            fixture: "a27_ob1_threadgroup",
            entry: "ob1_threadgroup",
        },
        // The visible-function-table kernel: `visible_function_table<WordOp> ops [[buffer(1)]]`
        // beside a plain `device uint *` at 0, which is the only shape in this tree whose
        // reflection can hold `MTLBindingTypeVisibleFunctionTable`. The kernel links no visible
        // function: it calls through the table's entries rather than naming one, so the pipeline
        // compiles from the library alone and the binding is the table's own argument.
        FunctionBindingsCase {
            name: "function-bindings-visible-function-table",
            fixture: "visible_table_caller",
            entry: "visible_table_dispatch",
        },
        // The explicit-imageblock kernel: `imageblock<Cell, imageblock_layout_explicit> img`
        // beside a `device uint4 *` at 0, which is the only shape in this tree that can reach
        // `MTLBindingTypeImageblock` or `MTLBindingTypeImageblockData`. Whether Metal lists a
        // compute kernel's imageblock as a binding at all is the measurement.
        FunctionBindingsCase {
            name: "function-bindings-imageblock",
            fixture: "a4_imageblock_extent",
            entry: "a4_imageblock_extent",
        },
        // The `MTLDataType` sweep. Six kernels hold one struct per MSL scalar, vector and matrix
        // type the enumeration names, in two shapes: behind a `device` pointer, where the derived
        // numbers are the pointee's size and alignment, and after a texture in an argument buffer,
        // where they are the member's offset and the struct's size. The split into three is the
        // fixture's: 22 arguments a kernel, which is a buffer-slot count every device has.
        FunctionBindingsCase {
            name: "function-bindings-data-types-pointer-floats",
            fixture: DATA_TYPES,
            entry: "encoder_types_plain_0",
        },
        FunctionBindingsCase {
            name: "function-bindings-data-types-pointer-integers",
            fixture: DATA_TYPES,
            entry: "encoder_types_plain_1",
        },
        FunctionBindingsCase {
            name: "function-bindings-data-types-pointer-narrow",
            fixture: DATA_TYPES,
            entry: "encoder_types_plain_2",
        },
        FunctionBindingsCase {
            name: "function-bindings-data-types-member-floats",
            fixture: DATA_TYPES,
            entry: "encoder_types_tail_0",
        },
        FunctionBindingsCase {
            name: "function-bindings-data-types-member-integers",
            fixture: DATA_TYPES,
            entry: "encoder_types_tail_1",
        },
        FunctionBindingsCase {
            name: "function-bindings-data-types-member-narrow",
            fixture: DATA_TYPES,
            entry: "encoder_types_tail_2",
        },
        // The argument buffer whose members are the resource types and the arrays of them:
        // `MTLDataTypeArray` over a texture, a sampler, a pointer, an inline constant and a
        // struct, each with the `stride` and `argumentIndexStride` Metal derived, beside an
        // indirect command buffer, a compute pipeline state, a depth texture and a nested
        // argument buffer. Its second argument states no `[[id]]` at all, so every index in it is
        // Metal's own assignment.
        FunctionBindingsCase {
            name: "function-bindings-data-types-arrays",
            fixture: "reflection/argument_buffers",
            entry: "reflection_argument_buffers",
        },
        // Ten argument buffers, each one two-element array of structs: which arrays Metal gives
        // stride 0 (resource-only elements) and which their element's size. Neither number is in
        // the source, and the two answers differ by the element's own composition alone.
        FunctionBindingsCase {
            name: "function-bindings-data-types-array-strides",
            fixture: "reflection/encoder_arrays",
            entry: "encoder_arrays",
        },
        // Thirteen texture types over four component types, where `textureDataType` is derived
        // from a template parameter the binding never names: `MTLDataTypeFloat`,
        // `MTLDataTypeHalf`, `MTLDataTypeUInt` and `MTLDataTypeInt` off `texture2d<float>`,
        // `texture2d<half>`, `texture2d<uint>` and `texture3d<int>`.
        FunctionBindingsCase {
            name: "function-bindings-data-types-texture-components",
            fixture: "reflection/textures",
            entry: "reflection_textures",
        },
        // A visible function table and an array of two inside an argument buffer, beside an
        // inline constant: `MTLDataTypeVisibleFunctionTable` as a member's and an array element's
        // type rather than as a binding's.
        FunctionBindingsCase {
            name: "function-bindings-data-types-tables",
            fixture: "reflection/tables_in_arguments",
            entry: "reflection_tables_in_arguments",
        },
    ]
}

impl FunctionBindingsCase {
    fn loaded(&self) -> Fixture {
        Fixture {
            name: self.fixture,
            entry: self.entry,
        }
    }
}

impl Case for FunctionBindingsCase {
    fn name(&self) -> &'static str {
        self.name
    }

    fn metal_source(&self) -> MetalSource {
        MetalSource::Run
    }

    fn shaders(&self) -> Result<Vec<ShaderRef>, String> {
        let fixture = self.loaded();
        [RUNTIME_TEXT, METAL_TEXT]
            .into_iter()
            .map(|extension| {
                Ok(ShaderRef {
                    fixture: format!("{}{extension}", self.fixture),
                    entry: self.entry.to_owned(),
                    source_sha256: crate::sha256_hex(fixture.text(extension)?.as_bytes()),
                })
            })
            .collect()
    }

    fn inputs(&self) -> Result<Vec<Input>, String> {
        let fixture = self.loaded();
        Ok(vec![
            Input {
                name: "air".to_owned(),
                digest: Digest::sha256(fixture.text(RUNTIME_TEXT)?.as_bytes()),
            },
            Input {
                name: "air-typed".to_owned(),
                digest: Digest::sha256(fixture.text(METAL_TEXT)?.as_bytes()),
            },
            // The entry point is the whole of the rest of what the case feeds: the reflection is a
            // fact about one function of the library.
            Input::bytes("entry", self.entry.as_bytes()),
        ])
    }

    fn run(&self, side: &mut dyn Side, sites: &mut Sites) -> Result<(), Abort> {
        let stop = |at: &str, why: String| Abort {
            at: at.to_owned(),
            failure: Failure::named("FixtureNotRead", why),
        };
        let loaded = self.loaded().loaded().map_err(|why| stop("library", why))?;
        let library = crate::case::made(sites, "library", side.library(&loaded.source()))?;
        match side.compute_pipeline_bindings(library, self.entry) {
            Ok(bindings) => {
                sites.push(bindings_node(BINDINGS, &bindings));
                Ok(())
            }
            Err(failure) => {
                sites.push(Node::leaf(BINDINGS, Observed::value(failure.recorded())));
                Err(Abort {
                    at: BINDINGS.to_owned(),
                    failure,
                })
            }
        }
    }
}
