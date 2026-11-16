//! The hasher the SPIR-V interners key their lookup indexes with.
//!
//! [`Types`](super::types::Types) and [`Consts`](super::constant::Consts) are insertion-ordered
//! lists with a hash index that is only ever looked up in, never iterated, and whose keys are the
//! interned values themselves — a `Type` with its members, a `Constant` with its operand list. The
//! standard library's SipHash-1-3 is chosen to make collisions unforgeable, which an internal
//! interner needs nothing of, and it costs a round per eight bytes of every lookup.
//!
//! [`FxHasher`] is the multiply-xor hash rustc uses for the same job: one rotate, one xor and one
//! multiply per eight bytes. It is fixed, not seeded per process, so a run's bucket order is the
//! same on every host — the standard hasher's is not, which is why no output may depend on either.
//!
//! The AIR front end keeps the same hasher for its own interners (`air::ir::intern`); the two
//! crates share no private module, and neither copy is published surface.

use std::hash::{BuildHasherDefault, Hasher};

/// The multiplier: the odd 64-bit approximation of the golden ratio, as rustc's FxHash uses.
const K: u64 = 0x517c_c1b7_2722_0a95;

/// A fast, deterministic multiply-xor hasher. Not collision-resistant; for internal tables only.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct FxHasher {
    hash: u64,
}

impl FxHasher {
    /// Fold one word in.
    #[inline]
    fn add(&mut self, word: u64) {
        self.hash = (self.hash.rotate_left(5) ^ word).wrapping_mul(K);
    }
}

impl Hasher for FxHasher {
    #[inline]
    fn write(&mut self, bytes: &[u8]) {
        let mut rest = bytes;
        while let Some((word, tail)) = rest.split_first_chunk::<8>() {
            self.add(u64::from_le_bytes(*word));
            rest = tail;
        }
        if !rest.is_empty() {
            // Fewer than eight bytes left, so every shift is under 64: the tail is the low bytes
            // of one word and the rest is zero.
            let mut word = 0u64;
            for (i, &b) in rest.iter().enumerate() {
                word |= u64::from(b) << (i * 8);
            }
            self.add(word);
        }
    }

    #[inline]
    fn write_u8(&mut self, v: u8) {
        self.add(u64::from(v));
    }

    #[inline]
    fn write_u16(&mut self, v: u16) {
        self.add(u64::from(v));
    }

    #[inline]
    fn write_u32(&mut self, v: u32) {
        self.add(u64::from(v));
    }

    #[inline]
    fn write_u64(&mut self, v: u64) {
        self.add(v);
    }

    #[inline]
    fn write_u128(&mut self, v: u128) {
        self.add(v as u64);
        self.add((v >> 64) as u64);
    }

    #[inline]
    fn write_usize(&mut self, v: usize) {
        self.add(v as u64);
    }

    #[inline]
    fn finish(&self) -> u64 {
        self.hash
    }
}

/// A `HashMap` keyed with [`FxHasher`]: the interners' lookup index.
pub(crate) type FxHashMap<K, V> = std::collections::HashMap<K, V, BuildHasherDefault<FxHasher>>;

#[cfg(test)]
mod tests {
    use super::{FxHashMap, FxHasher};
    use std::hash::{BuildHasher, BuildHasherDefault, Hasher};

    /// Equal keys hash equal, which is all a map requires of a hasher, at every length through a
    /// whole word and past it (the tail is padded, so the lengths 1..8 take the padding path).
    #[test]
    fn equal_keys_hash_equal() {
        let bytes: Vec<u8> = (0u32..40).map(|i| (i * 7 + 3) as u8).collect();
        let b: BuildHasherDefault<FxHasher> = BuildHasherDefault::default();
        for len in 0..=bytes.len() {
            let key = &bytes[..len];
            assert_eq!(b.hash_one(key), b.hash_one(&key.to_vec()[..]), "len {len}");
            let mut once = FxHasher::default();
            once.write(key);
            let mut again = FxHasher::default();
            again.write(key);
            assert_eq!(once.finish(), again.finish(), "len {len}");
        }
    }

    /// The hash of a value does not change between runs or builds: the state starts at zero and no
    /// seed enters it. A table keyed this way is therefore ordered the same everywhere.
    #[test]
    fn the_hash_is_fixed() {
        let b: BuildHasherDefault<FxHasher> = BuildHasherDefault::default();
        assert_eq!(b.hash_one(0u64), 0);
        assert_eq!(b.hash_one(1u64), super::K);
        let mut h = FxHasher::default();
        h.write(b"air.buffer");
        assert_eq!(h.finish(), 0xa3c5_22f6_ff18_5a7b);
    }

    /// The map behaves as a map: distinct keys stay distinct and equal keys collide.
    #[test]
    fn the_map_separates_keys() {
        let mut m: FxHashMap<Vec<u8>, u32> = FxHashMap::default();
        for i in 0u32..512 {
            m.insert(i.to_le_bytes().to_vec(), i);
        }
        assert_eq!(m.len(), 512);
        for i in 0u32..512 {
            assert_eq!(m.get(i.to_le_bytes().as_slice()), Some(&i));
        }
    }
}
