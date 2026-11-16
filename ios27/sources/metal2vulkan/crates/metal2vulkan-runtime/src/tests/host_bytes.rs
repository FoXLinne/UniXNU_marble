//! Bytes read through a pointer (`host_bytes.rs`): copied out once, the rows a copy packs straight
//! from the pointer, exactly as from a slice.

use super::super::copy_plan::HostLayout;
use super::super::host_bytes::HostBytes;
use std::ptr::NonNull;

/// Host bytes over `bytes`, as a caller with memory of its own makes them.
fn over(bytes: &[u8]) -> HostBytes<'_> {
    // SAFETY: `bytes` is a live slice, readable, which nothing writes while the test reads it.
    unsafe { HostBytes::new(NonNull::from(bytes).cast(), bytes.len()) }
}

/// Rows packed from a pointer are the rows packed from the slice: strided rows and images, a
/// pitch past the row, the last row ending at the bytes' end.
#[test]
fn rows_packed_from_a_pointer_are_the_slices() {
    let layout = HostLayout {
        row_bytes: 12,
        rows: 3,
        images: 2,
        bytes_per_row: 20,
        bytes_per_image: 70,
    };
    let len = layout.host_len().unwrap();
    let host: Vec<u8> = (0..len).map(|i| (i * 7 + 3) as u8).collect();
    let mut from_slice = vec![0; layout.packed_len() as usize];
    layout.pack(&host, &mut from_slice);
    for bytes in [over(&host), HostBytes::from_slice(&host)] {
        let mut from_pointer = vec![0xee; layout.packed_len() as usize];
        layout.pack_from(bytes, &mut from_pointer);
        assert_eq!(from_pointer, from_slice);
    }
}

/// A copy reads exactly the bytes asked for, and the whole copy every byte; one past the length
/// panics before reading anything.
#[test]
fn copies_read_their_bytes_and_stop_at_the_length() {
    let host: Vec<u8> = (0..32).collect();
    let bytes = over(&host);
    assert_eq!((bytes.len(), bytes.is_empty()), (32, false));
    assert_eq!(bytes.to_vec(), host);
    let mut part = [0; 5];
    bytes.copy_into(27, &mut part);
    assert_eq!(part, [27, 28, 29, 30, 31]);
    bytes.copy_into(32, &mut []);
    let past = std::panic::catch_unwind(|| bytes.copy_into(28, &mut [0; 5]));
    assert!(past.is_err(), "a copy past the length");
    let empty = HostBytes::from_slice(&[]);
    assert!(empty.is_empty() && empty.to_vec().is_empty());
}
