// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWINBOOT_MEMORY_WINDOW_H
#define DARWINBOOT_MEMORY_WINDOW_H

// A window with excluded ranges.
//
// XNU manages one span of physical addresses [PhysicalBase, PhysicalBase + MemorySize). When the device tree
// carries /chosen/darwinboot-memory-excluded, the span may cover more than one run of free memory: the loader
// writes into that property the ranges of the span that XNU must not use, as ascending (base, size) pairs of
// 64-bit values with unused slots zeroed. A range is excluded when the final UEFI memory map describes it as
// anything but free memory or loader memory, or does not describe it at all (a gap between two DRAM banks).
//
// Memory of the boot services types stays excluded. The UEFI specification returns it to the operating system at
// ExitBootServices, but a firmware driver whose device still performs DMA would then write into pages XNU has
// handed out, and the loader cannot observe whether every device was stopped.
//
// The first part of the span, the head, is one allocation: the loaded image followed by the room XNU needs for
// the tables it places directly behind the image before it reads the excluded ranges into its page allocator.

// Room behind the loaded image that holds no excluded range. XNU's physical-to-virtual tables, page attribute
// table and translation tables take about 1/320 of the span plus a few megabytes; XNU checks the same rule.
#define DARWIN_WINDOW_RESERVE(Size)  ((((Size) / 64) > 0x2000000ULL) ? ((Size) / 64) : 0x2000000ULL)

// Types the operating system owns after ExitBootServices without any firmware device having used them for DMA.
STATIC
BOOLEAN
DarwinMemoryUsable (UINT32 Type)
{
  return (Type == EfiConventionalMemory) || (Type == EfiLoaderCode) || (Type == EfiLoaderData);
}

// The byte range of a descriptor that counts: usable memory, or free memory only. FALSE for every other
// descriptor and for one whose size leaves the address space.
STATIC
BOOLEAN
DarwinDescriptorRange (CONST EFI_MEMORY_DESCRIPTOR *Descriptor, BOOLEAN FreeOnly, UINT64 *Start, UINT64 *End)
{
  if ((FreeOnly ? (Descriptor->Type != EfiConventionalMemory) : !DarwinMemoryUsable (Descriptor->Type)) ||
      (Descriptor->NumberOfPages == 0) ||
      (Descriptor->NumberOfPages > (MAX_UINT64 - Descriptor->PhysicalStart) / EFI_PAGE_SIZE)) {
    return FALSE;
  }
  *Start = Descriptor->PhysicalStart;
  *End = Descriptor->PhysicalStart + Descriptor->NumberOfPages * EFI_PAGE_SIZE;
  return TRUE;
}

// The first address at or above Address, and not above Limit, that no counted descriptor covers. The UEFI map
// may list adjacent ranges separately and in any order, so the scan repeats until nothing extends the run.
STATIC
UINT64
DarwinRunEnd (CONST VOID *Map, UINTN Size, UINTN Stride, BOOLEAN FreeOnly, UINT64 Address, UINT64 Limit)
{
  UINTN    Offset;
  UINT64   Start;
  UINT64   End;
  BOOLEAN  Extended;

  do {
    Extended = FALSE;
    for (Offset = 0; (Offset + Stride <= Size) && (Address < Limit); Offset += Stride) {
      if (DarwinDescriptorRange ((CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map + Offset), FreeOnly, &Start, &End) &&
          (Start <= Address) && (Address < End)) {
        Address = End;
        Extended = TRUE;
      }
    }
  } while (Extended && (Address < Limit));
  return (Address < Limit) ? Address : Limit;
}

// The lowest address at or above Address that usable memory covers, or Limit when there is none below it.
STATIC
UINT64
DarwinUsableNext (CONST VOID *Map, UINTN Size, UINTN Stride, UINT64 Address, UINT64 Limit)
{
  UINTN   Offset;
  UINT64  Start;
  UINT64  End;

  for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
    if (DarwinDescriptorRange ((CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map + Offset), FALSE, &Start, &End) &&
        (End > Address) && (Start < Limit)) {
      Limit = (Start > Address) ? Start : Address;
    }
  }
  return Limit;
}

// Usable bytes inside [Base, Base + Size).
STATIC
UINT64
DarwinWindowUsable (CONST VOID *Map, UINTN MapSize, UINTN Stride, UINT64 Base, UINT64 Size)
{
  UINT64  Limit;
  UINT64  Cursor;
  UINT64  End;
  UINT64  Usable;

  Limit = Base + Size;
  Usable = 0;
  for (Cursor = Base; Cursor < Limit; Cursor = End) {
    Cursor = DarwinUsableNext (Map, MapSize, Stride, Cursor, Limit);
    End = DarwinRunEnd (Map, MapSize, Stride, FALSE, Cursor, Limit);
    Usable += End - Cursor;
  }
  return Usable;
}

// The start of the best window of Size bytes: a free run that holds the Head bytes at the address the kernel's
// link address requires (Offset inside a Block), ranked by the usable memory inside the window, then by the lower
// address. Only starts ranked after (SkipUsable, SkipAddress) count, so a caller whose allocation at one start
// failed can ask for the next; MAX_UINT64 as SkipUsable ranks before every start. Returns 0 when there is none.
STATIC
UINT64
DarwinWindowStart (
  CONST VOID  *Map,
  UINTN       MapSize,
  UINTN       Stride,
  UINT64      Block,
  UINT64      Offset,
  UINT64      Head,
  UINT64      Size,
  UINT64      SkipUsable,
  UINT64      SkipAddress,
  UINT64      *Usable
  )
{
  UINTN   Index;
  UINT64  Start;
  UINT64  End;
  UINT64  Candidate;
  UINT64  Bytes;
  UINT64  Best;

  Best = 0;
  *Usable = 0;
  if ((Head == 0) || (Head > Size)) { return 0; }
  for (Index = 0; Index + Stride <= MapSize; Index += Stride) {
    if (!DarwinDescriptorRange ((CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map + Index), TRUE, &Start, &End) ||
        (Start > MAX_UINT64 - 2 * Block)) { continue; }
    Candidate = ((Start + Block - 1) & ~(Block - 1)) + Offset;
    if ((Candidate >= End) || (Candidate > MAX_UINT64 - Size) ||
        (DarwinRunEnd (Map, MapSize, Stride, TRUE, Candidate, Candidate + Head) != Candidate + Head)) { continue; }
    Bytes = DarwinWindowUsable (Map, MapSize, Stride, Candidate, Size);
    if ((Bytes > SkipUsable) || ((Bytes == SkipUsable) && (Candidate <= SkipAddress))) { continue; }
    if ((Best == 0) || (Bytes > *Usable) || ((Bytes == *Usable) && (Candidate < Best))) {
      Best = Candidate;
      *Usable = Bytes;
    }
  }
  return Best;
}

STATIC UINT64 DarwinRangeBase (CONST UINT8 *Table, UINTN Index) { return ReadUnaligned64 ((CONST UINT64 *)(Table + 16 * Index)); }
STATIC UINT64 DarwinRangeSize (CONST UINT8 *Table, UINTN Index) { return ReadUnaligned64 ((CONST UINT64 *)(Table + 16 * Index + 8)); }

STATIC
VOID
DarwinRangeSet (UINT8 *Table, UINTN Index, UINT64 Base, UINT64 Size)
{
  WriteUnaligned64 ((UINT64 *)(Table + 16 * Index), Base);
  WriteUnaligned64 ((UINT64 *)(Table + 16 * Index + 8), Size);
}

// Write the excluded ranges of [Base, Base + Size) into Table, which holds Capacity pairs and is not necessarily
// aligned (a device tree property). When the window has more excluded ranges than the table holds, the two
// neighbours with the least usable memory between them become one range: that memory is given up, and no
// excluded byte is ever left out. Unused pairs are zeroed. Returns the number of ranges and their bytes.
STATIC
UINTN
DarwinWindowExclusions (
  CONST VOID  *Map,
  UINTN       MapSize,
  UINTN       Stride,
  UINT64      Base,
  UINT64      Size,
  UINT8       *Table,
  UINTN       Capacity,
  UINT64      *Excluded
  )
{
  UINT64  Limit;
  UINT64  Cursor;
  UINT64  Next;
  UINT64  Gap;
  UINT64  Least;
  UINTN   Count;
  UINTN   Index;
  UINTN   Join;

  Limit = Base + Size;
  Count = 0;
  *Excluded = 0;
  for (Cursor = Base; (Cursor < Limit) && (Capacity != 0); Cursor = Next) {
    Cursor = DarwinRunEnd (Map, MapSize, Stride, FALSE, Cursor, Limit);
    if (Cursor >= Limit) { break; }
    Next = DarwinUsableNext (Map, MapSize, Stride, Cursor, Limit);
    if (Count < Capacity) {
      DarwinRangeSet (Table, Count++, Cursor, Next - Cursor);
      continue;
    }
    Join = Capacity - 1;
    Least = Cursor - (DarwinRangeBase (Table, Join) + DarwinRangeSize (Table, Join));
    for (Index = 0; Index + 1 < Capacity; Index++) {
      Gap = DarwinRangeBase (Table, Index + 1) - (DarwinRangeBase (Table, Index) + DarwinRangeSize (Table, Index));
      if (Gap < Least) { Least = Gap; Join = Index; }
    }
    if (Join == Capacity - 1) {
      DarwinRangeSet (Table, Join, DarwinRangeBase (Table, Join), Next - DarwinRangeBase (Table, Join));
      continue;
    }
    DarwinRangeSet (Table, Join, DarwinRangeBase (Table, Join),
                    DarwinRangeBase (Table, Join + 1) + DarwinRangeSize (Table, Join + 1) - DarwinRangeBase (Table, Join));
    for (Index = Join + 1; Index + 1 < Capacity; Index++) {
      DarwinRangeSet (Table, Index, DarwinRangeBase (Table, Index + 1), DarwinRangeSize (Table, Index + 1));
    }
    DarwinRangeSet (Table, Capacity - 1, Cursor, Next - Cursor);
  }
  for (Index = 0; Index < Capacity; Index++) {
    if (Index < Count) { *Excluded += DarwinRangeSize (Table, Index); }
    else { DarwinRangeSet (Table, Index, 0, 0); }
  }
  return Count;
}

// Free memory outside the window, for a kernel driver that manages memory of its own (a DRAM bank too far from the
// window for XNU's page array). Table holds Capacity (base, size) pairs like the excluded-range table; it gets the
// largest runs of EfiConventionalMemory that lie outside [Base, Base + Size), aligned inward to Align and at least
// Minimum bytes long, in ascending order, with unused pairs zeroed. Only free memory counts: after ExitBootServices
// nothing of the firmware uses it, while loader and boot services memory may still hold what the hand-off reads.
// Returns the number of ranges and their bytes.
STATIC
UINTN
DarwinOutsideRanges (
  CONST VOID  *Map,
  UINTN       MapSize,
  UINTN       Stride,
  UINT64      Base,
  UINT64      Size,
  UINT64      Align,
  UINT64      Minimum,
  UINT8       *Table,
  UINTN       Capacity,
  UINT64      *Bytes
  )
{
  UINTN   Offset;
  UINTN   Count;
  UINTN   Index;
  UINTN   Least;
  UINTN   Piece;
  UINT64  Cursor;
  UINT64  Start;
  UINT64  End;
  UINT64  Next;
  UINT64  From[2];
  UINT64  To[2];
  UINT64  Limit;
  BOOLEAN Found;

  Count = 0;
  *Bytes = 0;
  Limit = Base + Size;
  for (Cursor = 0; Capacity != 0; Cursor = End) {
    Found = FALSE;
    Next = MAX_UINT64;
    for (Offset = 0; Offset + Stride <= MapSize; Offset += Stride) {
      if (DarwinDescriptorRange ((CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map + Offset), TRUE, &Start, &End) &&
          (End > Cursor)) {
        if (Start < Cursor) { Start = Cursor; }
        if (Start < Next) { Next = Start; Found = TRUE; }
      }
    }
    if (!Found) { break; }
    Start = Next;
    End = DarwinRunEnd (Map, MapSize, Stride, TRUE, Start, MAX_UINT64);
    if (End <= Start) { break; }
    // The parts of the run below and above the window.
    From[0] = Start; To[0] = (End < Base) ? End : Base;
    From[1] = (Start > Limit) ? Start : Limit; To[1] = End;
    for (Piece = 0; Piece < 2; Piece++) {
      if (From[Piece] >= To[Piece]) { continue; }
      From[Piece] = (From[Piece] + Align - 1) & ~(Align - 1);
      To[Piece] &= ~(Align - 1);
      if ((From[Piece] >= To[Piece]) || (To[Piece] - From[Piece] < Minimum)) { continue; }
      if (Count < Capacity) {
        DarwinRangeSet (Table, Count++, From[Piece], To[Piece] - From[Piece]);
        continue;
      }
      Least = 0;
      for (Index = 1; Index < Capacity; Index++) {
        if (DarwinRangeSize (Table, Index) < DarwinRangeSize (Table, Least)) { Least = Index; }
      }
      if (To[Piece] - From[Piece] > DarwinRangeSize (Table, Least)) {
        DarwinRangeSet (Table, Least, From[Piece], To[Piece] - From[Piece]);
      }
    }
  }
  // Ascending by base.
  for (Index = 1; Index < Count; Index++) {
    for (Offset = Index; (Offset > 0) && (DarwinRangeBase (Table, Offset - 1) > DarwinRangeBase (Table, Offset)); Offset--) {
      Start = DarwinRangeBase (Table, Offset); End = DarwinRangeSize (Table, Offset);
      DarwinRangeSet (Table, Offset, DarwinRangeBase (Table, Offset - 1), DarwinRangeSize (Table, Offset - 1));
      DarwinRangeSet (Table, Offset - 1, Start, End);
    }
  }
  for (Index = 0; Index < Capacity; Index++) {
    if (Index < Count) { *Bytes += DarwinRangeSize (Table, Index); }
    else { DarwinRangeSet (Table, Index, 0, 0); }
  }
  return Count;
}

#endif
