// Owned fixture: a kernel whose only effect is the draws it encodes, of the primitive type the
// first word of `typed` holds, whatever it is: Metal's private values and those past them too.
// Thread 0 encodes command 0's draw of 6 vertices from 0; thread 1 command 1's draw of the six
// 16-bit indices from byte 16 of `typed`, once. The pipeline is the executing encoder's, which the
// commands inherit, and the vertex buffers the host's. `params` is bound here unread, so a case
// owns the buffers the host binds to the commands. Only executing the commands observes them.
// Loop-free.
#include <metal_stdlib>
using namespace metal;

struct Encoder {
    command_buffer commands;
};

kernel void icb_render_typed_encode(
    device const Encoder &encoder [[buffer(0)]],
    constant uint *params [[buffer(1)]],
    device const uint *typed [[buffer(2)]],
    uint tid [[thread_position_in_grid]])
{
    render_command command(encoder.commands, tid);
    primitive_type type = primitive_type(typed[0]);
    if (tid == 0) {
        command.draw_primitives(type, 0, 6, 1, 0);
    } else {
        command.draw_indexed_primitives(type, 6, (device const ushort *)(typed + 4), 1, 0, 0);
    }
}
