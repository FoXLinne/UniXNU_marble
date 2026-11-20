// SPDX-License-Identifier: MIT
//! Source-level signatures for Metal's visible and Core Image functions.

use metal2vulkan_air::abi::{CoreImageRole, VisibleParamKind};
use metal2vulkan_air::budget::{Budget, Meter};
use metal2vulkan_air::error::DiagName;
use metal2vulkan_air::ir::{Type, TypeId, TypeTable};
use metal2vulkan_air::metal::DataType;

pub(super) struct CallableType {
    pub(super) name: String,
    pub(super) data_type: DataType,
    pub(super) members: Vec<CallableType>,
}

pub(super) struct CallableArgument {
    pub(super) name: String,
    pub(super) ty: CallableType,
    pub(super) kind: u32,
}

pub(super) struct CallableSignature {
    pub(super) result: Option<CallableType>,
    pub(super) arguments: Vec<CallableArgument>,
}

// Metal reports callable value parameters as bindings of kind 18.
const VALUE: u32 = 18;
const TEXTURE: u32 = 2;
const SAMPLER: u32 = 3;
const STRUCT: u32 = 29;

fn numeric_type(name: &str) -> Option<DataType> {
    let name = name.strip_prefix("packed_").unwrap_or(name);
    let end = name.find(|c: char| c.is_ascii_digit()).unwrap_or(name.len());
    let (base, shape) = name.split_at(end);
    let family = match base {
        "float" => "Float",
        "half" => "Half",
        "bfloat" => "BFloat",
        "int" => "Int",
        "uint" => "UInt",
        "short" => "Short",
        "ushort" => "UShort",
        "char" => "Char",
        "uchar" => "UChar",
        "long" => "Long",
        "ulong" => "ULong",
        "bool" => "Bool",
        _ => return None,
    };
    DataType::from_name(&format!("MTLDataType{family}{shape}"))
}

fn describe(name: Option<&DiagName>, types: &TypeTable, id: TypeId, kind: u32) -> CallableType {
    let name = name.map(ToString::to_string).unwrap_or_default();
    let data_type = match kind {
        TEXTURE => DataType::Texture,
        SAMPLER => DataType::Sampler,
        _ => numeric_type(&name).unwrap_or_else(|| match types.get(id) {
            Some(Type::Struct { .. }) => DataType::Struct,
            Some(Type::Array { .. }) => DataType::Array,
            Some(Type::Ptr { .. }) => DataType::Pointer,
            _ => DataType::None,
        }),
    };
    CallableType { name, data_type, members: Vec::new() }
}

pub(super) fn signature(bytes: &[u8], name: &str) -> Result<CallableSignature, String> {
    let budget = Budget::standard();
    let decoded = metal2vulkan_air::decode(bytes, &mut Meter::new(&budget))
        .map_err(|error| format!("decode callable AIR: {error:?}"))?;
    let module = &decoded.module;
    let matches = |func| {
        module.func(func).and_then(|f| module.symbols().get(f.name)) == Some(name.as_bytes())
    };
    let visible = decoded.abi.visible_functions.iter().find(|entry| matches(entry.func));
    let core_image = decoded.abi.core_image_kernels.iter().find(|entry| matches(entry.func));
    let (func, result) = match (visible, core_image) {
        (Some(entry), _) => (entry.func, entry.outputs.first().map(|out| out.type_name.as_ref())),
        (None, Some(entry)) => (entry.func, entry.output.as_ref().map(|out| out.type_name.as_ref())),
        (None, None) => return Err(format!("no callable AIR signature for {name}")),
    };
    let types = module.types();
    let Some(Type::Function { ret, params, .. }) =
        module.func(func).and_then(|function| types.get(function.ty))
    else {
        return Err(format!("invalid callable AIR signature for {name}"));
    };
    let mut arguments = Vec::new();
    let append = |arguments: &mut Vec<CallableArgument>, index: u32, name: Option<&DiagName>, ty: Option<&DiagName>, kind: u32| {
        let id = *params.get(index as usize)
            .ok_or_else(|| format!("callable AIR parameter {index} is missing"))?;
        arguments.push(CallableArgument {
            name: name.map(ToString::to_string).unwrap_or_default(),
            ty: describe(ty, types, id, kind),
            kind,
        });
        Ok::<_, String>(())
    };
    if let Some(entry) = visible {
        let stitched_arguments = entry.stitching.as_ref().map(|info| info.arguments.as_slice());
        let inputs = entry.params.iter().filter(|param| matches!(param.kind, VisibleParamKind::Input { .. }));
        let matching_arguments = stitched_arguments.filter(|arguments| arguments.len() == inputs.clone().count());
        for (index, param) in inputs.enumerate() {
            if let VisibleParamKind::Input { type_name, name } = &param.kind {
                // The compiler lowers coreimage::Destination to a float2 in the callable
                // signature. The stitching load helper retains its original struct type.
                if let Some(argument) = matching_arguments.and_then(|arguments| arguments.get(index))
                    && let Some(loader) = argument.ty.helpers[0].and_then(|id| module.func(id))
                    && let Some(Type::Function { ret, .. }) = types.get(loader.ty)
                    && let Some(Type::Struct { name: Some(symbol), .. }) = types.get(*ret)
                    && module.symbols().get(*symbol) == Some(b"struct.coreimage::Destination")
                {
                    arguments.push(CallableArgument {
                        name: argument.name.as_ref().map(ToString::to_string).unwrap_or_default(),
                        ty: CallableType {
                            name: "coreimage::Destination".into(),
                            data_type: DataType::Struct,
                            members: vec![describe(type_name.as_ref(), types, params[param.param as usize], VALUE)],
                        },
                        kind: STRUCT,
                    });
                    continue;
                }
                let spelling = type_name.as_ref().map(ToString::to_string).unwrap_or_default();
                let kind = if spelling == "sampler" {
                    SAMPLER
                } else if spelling.starts_with("texture") || spelling.starts_with("depth") {
                    TEXTURE
                } else {
                    VALUE
                };
                append(&mut arguments, param.param, name.as_ref(), type_name.as_ref(), kind)?;
            }
        }
    } else if let Some(entry) = core_image {
        for param in &entry.params {
            let kind = match param.role {
                CoreImageRole::Texture { .. } => TEXTURE,
                CoreImageRole::Sampler => SAMPLER,
                _ => VALUE,
            };
            append(&mut arguments, param.param, param.name.as_ref(), param.type_name.as_ref(), kind)?;
        }
    }
    Ok(CallableSignature {
        result: result.map(|ty| describe(ty, types, *ret, VALUE)),
        arguments,
    })
}
