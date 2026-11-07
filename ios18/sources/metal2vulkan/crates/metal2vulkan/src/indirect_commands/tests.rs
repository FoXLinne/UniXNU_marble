use super::*;

const OWNED_STATE: Inheritance = Inheritance {
    pipeline: StateSource::Command,
    buffers: StateSource::Command,
};

const TEST_DESCRIPTOR: CommandDescriptor = CommandDescriptor::Compute {
    dispatch: ComputeDispatch::Both,
    max_buffer_bind_count: 32,
    max_threadgroup_memory_bind_count: 31,
    support_dynamic_attribute_stride: true,
};

mod buffer_views;
mod inheritance;
mod records;

/// Each resource ID's records start at an address of their own, `2^63 + id * 2^31`, above every
/// Metal address (below 2^63): ID 1's at `2^63 + 2^31`, the null ID's below it, in the IDs' order,
/// and the last ID's 2^62 bytes below 2^64, so records of any length a device holds fit after it.
#[test]
fn each_records_id_has_a_start_of_its_own_above_the_metal_addresses() {
    let stride = 1u64 << 31;
    assert_eq!(records_address(0), 1 << 63);
    assert_eq!(records_address(1), (1 << 63) + stride);
    assert_eq!(RECORDS_ID_LIMIT, 1 << 31);
    let last = RECORDS_ID_LIMIT - 1;
    assert!(u128::from(records_address(last)) + (1u128 << 62) <= 1u128 << 64);
    for id in [1, 2, 3, 65_536, 70_000, 1 << 26, (1 << 30) + 6, last] {
        assert_eq!(
            records_address(id) - records_address(id - 1),
            stride,
            "{id}"
        );
        assert_eq!(
            (records_address(id) - RECORDS_ADDRESS_BASE) >> RECORDS_ADDRESS_SHIFT,
            id
        );
    }
}
