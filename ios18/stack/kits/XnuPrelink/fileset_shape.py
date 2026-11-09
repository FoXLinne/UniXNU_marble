"""Compare code across compiler-inserted arm64e authentication checks.

This only normalizes analysis. Emitted instructions keep every native check.
"""
import struct

from kc_image import scan, signed


def checked_shape(code, pc):
    words = list(struct.unpack('<' + 'I' * (len(code) // 4), code))
    # A landing pad changes entry requirements, not the body being compared.
    # Native emission retains it; normalize only a leading BTI instruction.
    removed = {0} if words and words[0] in (0xd503241f, 0xd503245f, 0xd503249f, 0xd50324df) else set()
    replacements = {}
    for i in range(1, len(words)):
        # An authenticated call consumes x16 as its address discriminator.
        # Both compiler forms clobber x9/x16/x17 at the call boundary.
        if words[i] != 0xaa1003e9 or words[i - 1] & 0xffe00fff != 0xf8400e08:
            continue
        for j in range(i + 1, min(i + 6, len(words) - 2)):
            if (words[j] == 0xaa0903f1 and words[j + 1] & 0xffe0001f == 0xf2e00011
                    and words[j + 2] == 0xd73f0911):
                removed.update((i, j))
                replacements[j + 1] = (words[j + 1] & ~31) | 16
                replacements[j + 2] = 0xd73f0910
                break
            # Intervening argument copies must be independent of the call operands.
            word = words[j]
            if (word & 0xffe0ffe0 != 0xaa0003e0 or
                    word & 31 in (8, 9, 16, 17) or (word >> 16) & 31 in (8, 9, 16, 17)):
                break
    for i in range(len(words)):
        # mov x17,x16; xpac[di] x17; cmp x16,x17; b.eq +8;
        # brk #0xc47[02]. Only accept the complete compiler sequence.
        block = words[i:i + 5]
        if block in ([0xaa1003f1, 0xdac147f1, 0xeb11021f, 0x54000040, 0xd4388e40],
                     [0xaa1003f1, 0xdac143f1, 0xeb11021f, 0x54000040, 0xd4388e00]):
            # AUTIA/AUTDA x16,x17 must immediately precede the check.
            if i and words[i - 1] in (0xdac11230, 0xdac11a30):
                removed.update(range(i, i + 5))
        # Compiler's LR authentication check before a tail branch.
        if (words[i:i + 3] == [0xca1e07d0, 0xb6f00050, 0xd4388e20] and
                i + 3 < len(words) and words[i + 3] & 0xfc000000 == 0x14000000):
            removed.update(range(i, i + 3))

    calls, refs, shape = scan(code, pc)
    positions = {i * 4: j * 4 for j, i in enumerate(i for i in range(len(words)) if i not in removed)}
    normalized = []
    for i, word in enumerate(words):
        if i in removed:
            continue
        bits, shift = (26, 0) if word & 0x7c000000 == 0x14000000 else \
                      (19, 5) if word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000 else \
                      (14, 5) if word & 0x7e000000 == 0x36000000 else (0, 0)
        value = replacements.get(i, shape[i])
        if bits:
            mask = (1 << bits) - 1
            target = i * 4 + signed((word >> shift) & mask, bits) * 4
            if 0 <= target < len(code):
                if target not in positions:
                    # Another edge enters a check: it is not disposable padding.
                    return shape, {j * 4: j * 4 for j in range(len(words))}, calls, refs
                immediate = (positions[target] - positions[i * 4]) // 4
                value = (word & ~(mask << shift)) | ((immediate & mask) << shift)
        normalized.append(value)
    return normalized, positions, calls, refs


def relocation_free_leaf(code, pc):
    """Exact body key for a returning leaf whose instructions do not depend on its PC."""
    words = struct.unpack('<' + 'I' * (len(code) // 4), code)
    if not words or len(words) > 32:
        return None
    returns = (0xd65f03c0, 0xd65f0bff, 0xd65f0fff)
    for index, word in enumerate(words):
        if word & 0x1f000000 == 0x10000000 or word & 0x3b000000 == 0x18000000:
            return None  # ADR, ADRP and literal loads use the instruction address.
        if word & 0xfc000000 == 0x94000000:
            return None  # A call changes LR and can leave the function.
        if word & 0xfe000000 == 0xd6000000 and word not in returns:
            return None  # Computed branches and exception returns are not leaves.
        bits, shift = (26, 0) if word & 0xfc000000 == 0x14000000 else \
                      (19, 5) if word & 0xff000000 == 0x54000000 or word & 0x7e000000 == 0x34000000 else \
                      (14, 5) if word & 0x7e000000 == 0x36000000 else (0, 0)
        if bits:
            target = index * 4 + signed((word >> shift) & ((1 << bits) - 1), bits) * 4
            if not 0 <= target < len(code):
                return None
    body, _, _, _ = checked_shape(code, pc)
    return tuple(body) if body and body[-1] in returns else None
