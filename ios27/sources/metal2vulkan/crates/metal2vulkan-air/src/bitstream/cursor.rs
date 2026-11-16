//! Bit-level reads over a byte slice, least-significant bit first (LLVM bitstream order).
//!
//! Every read is bounded by `limit`, the end of the innermost open block (or of the stream). A
//! read that would cross it fails with [`BitstreamError::UnexpectedEnd`], so a record can never
//! run into its parent's bytes.

use crate::error::BitstreamError;

/// Largest fixed-width field (bits).
pub(super) const MAX_FIXED_WIDTH: u32 = 64;
/// Largest VBR chunk width (bits), as in LLVM.
pub(super) const MAX_VBR_WIDTH: u32 = 32;

#[derive(Debug)]
pub(super) struct Cursor<'a> {
    data: &'a [u8],
    pos: u64,
    limit: u64,
}

impl<'a> Cursor<'a> {
    /// A cursor over all of `data`, starting at bit 0.
    pub(super) fn new(data: &'a [u8]) -> Cursor<'a> {
        let limit = (data.len() as u64).saturating_mul(8);
        Cursor {
            data,
            pos: 0,
            limit,
        }
    }

    /// The current bit position from the start of the stream.
    pub(super) fn pos(&self) -> u64 {
        self.pos
    }

    /// The end of the readable range (bits).
    pub(super) fn limit(&self) -> u64 {
        self.limit
    }

    /// The length of the whole stream (bits).
    pub(super) fn len_bits(&self) -> u64 {
        (self.data.len() as u64).saturating_mul(8)
    }

    /// Bits left before `limit`.
    pub(super) fn remaining(&self) -> u64 {
        self.limit.saturating_sub(self.pos)
    }

    /// Narrow or restore the readable range. Callers keep `limit <= len_bits()`.
    pub(super) fn set_limit(&mut self, limit: u64) {
        self.limit = limit.min(self.len_bits());
    }

    /// Move to `pos`, which must be within the readable range.
    pub(super) fn seek(&mut self, pos: u64) -> Result<(), BitstreamError> {
        if pos > self.limit {
            return Err(BitstreamError::UnexpectedEnd);
        }
        self.pos = pos;
        Ok(())
    }

    /// Read a `width`-bit field (`width <= 64`).
    ///
    /// A field whose bits all lie in the eight bytes at its own byte is taken from one
    /// little-endian word, which is every field but those in the last seven bytes of the stream
    /// ([`Cursor::spanning`] reads those a byte at a time). Both forms read the same bits: the
    /// stream is least-significant-bit first, so a `u64` read at the field's byte holds the
    /// field's bits at `pos % 8`, which is what the byte loop assembles.
    #[inline]
    pub(super) fn fixed(&mut self, width: u32) -> Result<u64, BitstreamError> {
        if width > MAX_FIXED_WIDTH {
            return Err(BitstreamError::BadAbbrev);
        }
        let end = self
            .pos
            .checked_add(u64::from(width))
            .ok_or(BitstreamError::UnexpectedEnd)?;
        if end > self.limit {
            return Err(BitstreamError::UnexpectedEnd);
        }
        // `pos % 8 < 8`, so the conversion is exact.
        let shift = (self.pos % 8) as u32;
        let at = usize::try_from(self.pos / 8).map_err(|_| BitstreamError::UnexpectedEnd)?;
        let value = match self.data.get(at..).and_then(|rest| rest.first_chunk::<8>()) {
            Some(word) if shift + width <= 64 => {
                (u64::from_le_bytes(*word) >> shift) & Cursor::mask(width)
            }
            _ => self.spanning(at, shift, width)?,
        };
        self.pos = end;
        Ok(value)
    }

    /// The low `width` bits set (`width <= 64`).
    fn mask(width: u32) -> u64 {
        if width == MAX_FIXED_WIDTH {
            u64::MAX
        } else {
            (1u64 << width) - 1
        }
    }

    /// A `width`-bit field at bit `shift` of byte `at`, assembled one byte at a time: the form for
    /// a field the stream's last word does not hold whole. The caller has checked that the field
    /// ends within `limit`.
    fn spanning(&self, at: usize, shift: u32, width: u32) -> Result<u64, BitstreamError> {
        let mut value = 0u64;
        let mut got = 0u32;
        let mut at = at;
        let mut shift = shift;
        while got < width {
            let byte = self
                .data
                .get(at)
                .copied()
                .ok_or(BitstreamError::UnexpectedEnd)?;
            let take = (8 - shift).min(width - got);
            let bits = (u64::from(byte) >> shift) & ((1u64 << take) - 1);
            value |= bits << got;
            got += take;
            at += 1;
            shift = 0;
        }
        Ok(value)
    }

    /// Read a variable-width integer made of `width`-bit chunks (`2 <= width <= 32`). A value that
    /// does not fit 64 bits is [`BitstreamError::BadVbr`].
    pub(super) fn vbr(&mut self, width: u32) -> Result<u64, BitstreamError> {
        if !(2..=MAX_VBR_WIDTH).contains(&width) {
            return Err(BitstreamError::BadAbbrev);
        }
        let payload_bits = width - 1;
        let high = 1u64 << payload_bits;
        let mut value = 0u64;
        let mut shift = 0u32;
        loop {
            let chunk = self.fixed(width)?;
            let payload = chunk & (high - 1);
            if payload != 0 {
                if shift >= 64 || (payload << shift) >> shift != payload {
                    return Err(BitstreamError::BadVbr);
                }
                value |= payload << shift;
            }
            if chunk & high == 0 {
                return Ok(value);
            }
            shift += payload_bits;
            if shift >= 64 {
                // A further chunk could only carry bits beyond 64.
                return Err(BitstreamError::BadVbr);
            }
        }
    }

    /// A VBR value that must fit `u32`.
    pub(super) fn vbr32(&mut self, width: u32) -> Result<u32, BitstreamError> {
        let value = self.vbr(width)?;
        u32::try_from(value).map_err(|_| BitstreamError::ValueOutOfRange)
    }

    /// Skip to the next multiple of 32 bits.
    pub(super) fn align32(&mut self) -> Result<(), BitstreamError> {
        let aligned = self
            .pos
            .checked_add(31)
            .map(|p| p & !31)
            .ok_or(BitstreamError::UnexpectedEnd)?;
        self.seek(aligned)
    }

    /// Take `len` bytes at the current (byte-aligned) position.
    pub(super) fn bytes(&mut self, len: u64) -> Result<&'a [u8], BitstreamError> {
        if !self.pos.is_multiple_of(8) {
            return Err(BitstreamError::BadBlob);
        }
        let bits = len.checked_mul(8).ok_or(BitstreamError::UnexpectedEnd)?;
        if bits > self.remaining() {
            return Err(BitstreamError::UnexpectedEnd);
        }
        let start = usize::try_from(self.pos / 8).map_err(|_| BitstreamError::UnexpectedEnd)?;
        let len = usize::try_from(len).map_err(|_| BitstreamError::UnexpectedEnd)?;
        let end = start
            .checked_add(len)
            .ok_or(BitstreamError::UnexpectedEnd)?;
        let slice = self
            .data
            .get(start..end)
            .ok_or(BitstreamError::UnexpectedEnd)?;
        self.pos += bits;
        Ok(slice)
    }
}

#[cfg(test)]
mod tests {
    use super::{Cursor, MAX_FIXED_WIDTH};

    /// Bit `i` of the stream, least-significant bit of each byte first.
    fn bit(data: &[u8], i: u64) -> u64 {
        let byte = data[(i / 8) as usize];
        u64::from(byte >> (i % 8)) & 1
    }

    /// Every field of every width at every bit position of a stream, against the bits themselves.
    ///
    /// The buffer is long enough that most fields lie in the eight bytes at their own byte (the
    /// one-word form) and the last seven bytes do not (the byte-at-a-time form), so one pass
    /// covers both and holds them to the same answer. Exhaustive: 25 x 8 positions x 65 widths.
    #[test]
    fn every_field_reads_its_bits() {
        let data: Vec<u8> = (0u32..25).map(|i| (i * 37 + 11) as u8).collect();
        let bits = (data.len() as u64) * 8;
        let mut spanning = 0u32;
        for pos in 0..bits {
            for width in 0..=MAX_FIXED_WIDTH {
                if pos + u64::from(width) > bits {
                    continue;
                }
                let mut c = Cursor::new(&data);
                c.seek(pos).expect("in range");
                let got = c.fixed(width).expect("in range");
                let want = (0..u64::from(width)).fold(0u64, |v, i| v | (bit(&data, pos + i) << i));
                assert_eq!(got, want, "pos {pos} width {width}");
                assert_eq!(c.pos(), pos + u64::from(width));
                if (pos / 8) as usize + 8 > data.len() {
                    spanning += 1;
                }
            }
        }
        assert!(spanning > 0, "no field took the byte-at-a-time form");
    }

    /// A field the limit cuts short is refused, whichever form would read it.
    #[test]
    fn a_field_past_the_limit_is_refused() {
        let data = [0xffu8; 24];
        for limit in [1u64, 7, 64, 100, 191] {
            let mut c = Cursor::new(&data);
            c.set_limit(limit);
            c.seek(limit.saturating_sub(1)).expect("in range");
            assert!(c.fixed(1).is_ok());
            assert!(c.fixed(1).is_err(), "limit {limit}");
        }
    }
}
