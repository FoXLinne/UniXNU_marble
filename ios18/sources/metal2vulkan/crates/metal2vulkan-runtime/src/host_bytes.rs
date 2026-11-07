//! Bytes a call reads through a pointer ([`HostBytes`]), never through a reference: memory that
//! code outside the program's Rust (a guest's virtual CPUs, another process, a device) may write
//! while the call reads it, which no `&[u8]` may cover.
//!
//! - **Read once, by copies.** A call given `HostBytes` reads them only by copying them through
//!   the pointer into memory of the runtime's own (`ptr::copy_nonoverlapping`), each byte at most
//!   once, before it returns: everything it does with them after (a check of their values, a
//!   packing of rows) reads its own copy. It forms no reference over them and keeps nothing of
//!   the pointer.
//! - **Where the slice forms read them so too.** A slice the caller owns may be given as
//!   [`HostBytes::from_slice`]: the calls that take a slice read it the same way.

use std::marker::PhantomData;
use std::ptr::NonNull;

/// `length` bytes at a pointer, for a call to copy out once, as Metal reads a `const void *` and
/// a length (`replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:`,
/// `newBufferWithBytes:length:options:`): a runtime extension beside the calls that take a slice,
/// for memory that may change while it is read, which Metal reads as it is then. The call reads
/// each byte at most once, by copying it through the pointer into memory of its own, and keeps
/// nothing of the pointer once it returns (the module docs). `'a` bounds how long it may be given
/// to calls: a slice's borrow ([`HostBytes::from_slice`]), or what [`HostBytes::new`]'s caller
/// vouches for.
#[derive(Clone, Copy, Debug)]
pub struct HostBytes<'a> {
    pointer: NonNull<u8>,
    length: usize,
    bytes: PhantomData<&'a [u8]>,
}

// SAFETY: an address and a length; a call reads through it only while it runs, under the
// guarantee its maker gave (`HostBytes::new`, or a slice's borrow), by copies alone.
unsafe impl Send for HostBytes<'_> {}
// SAFETY: as above; a shared `HostBytes` answers its address and length alone.
unsafe impl Sync for HostBytes<'_> {}

impl<'a> HostBytes<'a> {
    /// The `length` bytes at `pointer`.
    ///
    /// # Safety
    ///
    /// For each call it is given to within `'a`, until that call returns:
    /// - the `length` bytes at `pointer` are mapped memory of this process, readable, and lie in
    ///   one allocation (for a `length` of 0 nothing is read);
    /// - no Rust code of this process writes them (that would be a data race). Code outside the
    ///   program's Rust may write them meanwhile (a guest's virtual CPUs, another process, a
    ///   device): the call then copies, for each byte, its value before or after the write,
    ///   which one unspecified, as Metal's read of changing memory is.
    pub unsafe fn new(pointer: NonNull<u8>, length: usize) -> HostBytes<'a> {
        HostBytes {
            pointer,
            length,
            bytes: PhantomData,
        }
    }

    /// The bytes of `bytes`, borrowed for `'a`: the slice forms of the calls, as [`HostBytes`].
    pub fn from_slice(bytes: &'a [u8]) -> HostBytes<'a> {
        HostBytes {
            pointer: NonNull::from(bytes).cast(),
            length: bytes.len(),
            bytes: PhantomData,
        }
    }

    /// Its first byte's address.
    pub fn pointer(&self) -> NonNull<u8> {
        self.pointer
    }

    /// How many bytes it holds.
    pub fn len(&self) -> usize {
        self.length
    }

    /// Whether it holds no byte.
    pub fn is_empty(&self) -> bool {
        self.length == 0
    }

    /// A copy of every byte, read once into memory of the runtime's own.
    pub(crate) fn to_vec(self) -> Vec<u8> {
        let mut copy = vec![0; self.length];
        self.copy_into(0, &mut copy);
        copy
    }

    /// Copy the bytes from byte `at` on into `into`, as many as it holds: each read once,
    /// through the pointer. Panics where they lie past its length.
    pub(crate) fn copy_into(self, at: usize, into: &mut [u8]) {
        let end = at
            .checked_add(into.len())
            .filter(|&end| end <= self.length)
            .expect("a copy of host bytes past their length");
        if into.is_empty() {
            return;
        }
        // SAFETY: bytes `at..end` lie inside the `length` bytes kept mapped and readable for this
        // call, which no Rust code writes meanwhile (`HostBytes::new`'s caller vouches for them,
        // or a slice's shared borrow, alive for `'a`, holds them); `into` is held as `&mut`, so
        // no shared borrow of a slice covers it, and memory `HostBytes::new` vouches for is
        // written by no Rust code, so neither overlaps them. Read as raw bytes through the
        // pointer, with no reference over them.
        unsafe {
            std::ptr::copy_nonoverlapping(
                self.pointer.as_ptr().add(at),
                into.as_mut_ptr(),
                end - at,
            );
        }
    }
}
