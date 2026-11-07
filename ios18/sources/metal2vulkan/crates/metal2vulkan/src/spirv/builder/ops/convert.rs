//! Conversion constructors: numeric conversions, pointer/integer conversions, and bitcasts.

use crate::spirv::builder::function::FunctionBuilder;
use crate::spirv::builder::ops::err;
use crate::spirv::error::{BResult, OperandRule};
use crate::spirv::ids::{TypeId, Value};
use crate::spirv::ir::{
    ConstBits, ConstKind, Constant, Contraction, ConvOp, Inst, StorageClass, Type, ValueOrigin,
};

/// The `half` bits of the `float` with bits `f` where it is exactly a zero, an infinity or a normal
/// `half`; `None` for a NaN, a value that needs rounding, and a `half` denormal.
fn exact_half_bits(f: u32) -> Option<u16> {
    let sign = ((f >> 16) & 0x8000) as u16;
    let exp = (f >> 23) & 0xFF;
    let man = f & 0x7F_FFFF;
    match (exp, man) {
        (0, 0) => Some(sign),
        (0xFF, 0) => Some(sign | 0x7C00),
        // A normal `half`: an unbiased exponent from -14 to 15 and ten significand bits.
        (113..=142, m) if m & 0x1FFF == 0 => {
            Some(sign | (((exp - 112) as u16) << 10) | (m >> 13) as u16)
        }
        _ => None,
    }
}

/// A bitcast endpoint: a numeric (non-`bool`) scalar or vector, or a `PhysicalStorageBuffer`
/// pointer, with its total bit width.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Cast {
    Num { bits: u32, int: bool, n: u8 },
    Ptr,
}

impl FunctionBuilder<'_> {
    /// A conversion of `a` to type `to`. The target type is the only input a conversion cannot
    /// derive; it is checked against the opcode:
    ///
    /// - `ConvertFToU`/`ConvertFToS`: float to unsigned (resp. any) integer of the same component
    ///   count; `ConvertSToF`/`ConvertUToF`: integer to float.
    /// - `UConvert` (unsigned result), `SConvert`, `FConvert`: same class and component count, a
    ///   different width.
    /// - `ConvertPtrToU`: `PhysicalStorageBuffer` pointer to a 64-bit integer scalar;
    ///   `ConvertUToPtr`: 64-bit integer scalar to such a pointer.
    /// - `Bitcast`: numeric scalars/vectors or `PhysicalStorageBuffer` pointers of equal total
    ///   width; a pointer converts only to another such pointer, a 64-bit integer, or a vector of
    ///   two 32-bit integers.
    ///
    /// An `FConvert` to 16 bits emits nothing where its value is exact: of a 32-bit float this
    /// builder widened from a 16-bit float of type `to`, that float (unless the 16-bit float is
    /// itself a narrowing, whose pair is a rounding to `half` kept whole); of a plain constant
    /// whose every lane is a zero, an infinity or a normal `half`, the 16-bit constant.
    pub fn convert(&mut self, op: ConvOp, a: Value, to: TypeId) -> BResult<Value> {
        self.convert_with(op, a, to, Contraction::Allowed)
    }

    /// An `FConvert` of `a` to type `to` decorated `NoContraction`: a conversion the device must
    /// perform as written. Vulkan assumes `AllowTransform` on a float operation not so decorated,
    /// under which a driver may drop a conversion to 16 bits that another widens back (Mesa's NIR
    /// folds `f2f32(f2f16(x))` to `x`).
    pub fn convert_no_contraction(&mut self, a: Value, to: TypeId) -> BResult<Value> {
        self.convert_with(ConvOp::FConvert, a, to, Contraction::Forbidden)
    }

    fn convert_with(
        &mut self,
        op: ConvOp,
        a: Value,
        to: TypeId,
        contract: Contraction,
    ) -> BResult<Value> {
        let o = op.to_spirv();
        let result = err(o, OperandRule::ResultType);
        // The widths of an `FConvert`'s operand and result.
        let mut float_widths = None;
        match op {
            ConvOp::ConvertFToU
            | ConvOp::ConvertFToS
            | ConvOp::ConvertSToF
            | ConvOp::ConvertUToF
            | ConvOp::UConvert
            | ConvOp::SConvert
            | ConvOp::FConvert => {
                let from_float = matches!(
                    op,
                    ConvOp::ConvertFToU | ConvOp::ConvertFToS | ConvOp::FConvert
                );
                let x = self.want(
                    o,
                    0,
                    a,
                    |n| {
                        if from_float {
                            n.is_float()
                        } else {
                            n.is_int()
                        }
                    },
                )?;
                let t = self.num_of(to)?.ok_or(result.clone())?;
                let ok = t.n == x.n
                    && match op {
                        ConvOp::ConvertFToU => t.is_uint(),
                        ConvOp::ConvertFToS => t.is_int(),
                        ConvOp::ConvertSToF | ConvOp::ConvertUToF => t.is_float(),
                        ConvOp::UConvert => t.is_uint() && t.width != x.width,
                        ConvOp::SConvert => t.is_int() && t.width != x.width,
                        ConvOp::FConvert => t.is_float() && t.width != x.width,
                        _ => false,
                    };
                if !ok {
                    return Err(result);
                }
                if op == ConvOp::FConvert {
                    float_widths = Some((x.width, t.width));
                }
            }
            ConvOp::ConvertUToPtr => {
                let x = self.want(o, 0, a, |n| n.is_int() && n.is_scalar())?;
                if x.width != 64 {
                    return Err(err(o, OperandRule::Width(0)));
                }
                if !self.is_psb_ptr(to)? {
                    return Err(result);
                }
            }
            ConvOp::Bitcast => {
                let from = self
                    .cast_of(self.value_ty(a)?)?
                    .ok_or(err(o, OperandRule::Category(0)))?;
                let into = self.cast_of(to)?.ok_or(result.clone())?;
                let ok = match (from, into) {
                    (Cast::Ptr, Cast::Ptr) => true,
                    (Cast::Ptr, Cast::Num { bits, int, n })
                    | (Cast::Num { bits, int, n }, Cast::Ptr) => int && bits == 64 && n <= 2,
                    (Cast::Num { bits: x, .. }, Cast::Num { bits: y, .. }) => x == y,
                };
                if !ok {
                    return Err(result);
                }
            }
        }
        match float_widths {
            // A 16-bit float widened and narrowed back is that float, exactly; a rounding's pair
            // (a narrowing widened back) is not taken apart (`note_widened_half`).
            Some((32, 16)) => {
                if let Some(h) = self
                    .widened_half_of(a)
                    .filter(|h| self.value_ty(*h).ok() == Some(to))
                {
                    return Ok(h);
                }
                if let Some(c) = self.exact_half_const(a, to)? {
                    return Ok(c);
                }
                let y = self.emit(Inst::Convert { op, a, contract }, to)?;
                self.note_narrowed_float(y);
                return Ok(y);
            }
            Some((16, 32)) => {
                let y = self.emit(Inst::Convert { op, a, contract }, to)?;
                self.note_widened_half(y, a);
                return Ok(y);
            }
            _ => {}
        }
        self.emit(Inst::Convert { op, a, contract }, to)
    }

    /// The 16-bit float constant of type `to` (a scalar or vector) that 32-bit float constant `a`
    /// holds exactly, where every lane of it is a zero, an infinity or a normal `half`; `None`
    /// where `a` is no plain constant or a lane is a NaN or needs rounding, or a `half` denormal
    /// (each the device's conversion's to answer).
    fn exact_half_const(&mut self, a: Value, to: TypeId) -> BResult<Option<Value>> {
        let ValueOrigin::Const(c) = self.origin(a)? else {
            return Ok(None);
        };
        let lanes: Vec<u64> = match &self.mb().consts().get(c)?.kind {
            ConstKind::Plain(Constant::Scalar { bits, .. }) => vec![bits.0],
            ConstKind::Plain(Constant::Null { .. }) => vec![0; self.lanes_of(to)?],
            ConstKind::Plain(Constant::Composite { parts, .. }) => {
                let mut lanes = Vec::with_capacity(parts.len());
                for p in parts.iter() {
                    match &self.mb().consts().get(*p)?.kind {
                        ConstKind::Plain(Constant::Scalar { bits, .. }) => lanes.push(bits.0),
                        ConstKind::Plain(Constant::Null { .. }) => lanes.push(0),
                        _ => return Ok(None),
                    }
                }
                lanes
            }
            _ => return Ok(None),
        };
        let Some(halves) = lanes
            .iter()
            .map(|b| u32::try_from(*b).ok().and_then(exact_half_bits))
            .collect::<Option<Vec<u16>>>()
        else {
            return Ok(None);
        };
        let e = self.module().ty(Type::Float { width: 16 })?;
        let mut parts = Vec::with_capacity(halves.len());
        for h in &halves {
            parts.push(self.module().constant(Constant::Scalar {
                ty: e,
                bits: ConstBits(u64::from(*h)),
            })?);
        }
        let k = match parts.as_slice() {
            [one] if to == e => *one,
            _ => self.module().constant(Constant::Composite {
                ty: to,
                parts: parts.into(),
            })?,
        };
        self.module().const_value(k).map(Some)
    }

    /// The lane count of scalar or vector type `ty`.
    fn lanes_of(&self, ty: TypeId) -> BResult<usize> {
        Ok(match self.mb().types().get(ty)? {
            Type::Vector { len, .. } => usize::from(*len),
            _ => 1,
        })
    }

    fn is_psb_ptr(&self, ty: TypeId) -> BResult<bool> {
        Ok(matches!(
            self.mb().types().get(ty)?,
            Type::Pointer {
                class: StorageClass::PhysicalStorageBuffer,
                ..
            }
        ))
    }

    fn cast_of(&self, ty: TypeId) -> BResult<Option<Cast>> {
        if self.is_psb_ptr(ty)? {
            return Ok(Some(Cast::Ptr));
        }
        Ok(match self.num_of(ty)? {
            Some(x) if !x.is_bool() => Some(Cast::Num {
                bits: u32::from(x.width) * u32::from(x.n),
                int: x.is_int(),
                n: x.n,
            }),
            _ => None,
        })
    }
}
