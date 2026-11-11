// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWINBOOT_LOADER_INPUTS_H
#define DARWINBOOT_LOADER_INPUTS_H

// The memory of the loader's inputs.
//
// Two inputs reach the loader in memory that the firmware describes as neither free nor loader memory:
//
//  - the payload RAM disk. The first-stage boot loader places it in DRAM and names its range in the device tree
//    it hands to the firmware (/chosen linux,initrd-start and linux,initrd-end); the firmware's SEC phase keeps
//    that tree's address, the entry argument, in a GUID HOB, and a firmware block device serves the range.
//  - the boot entry's initrd: a pool buffer the boot manager filled from the payload and owns for the whole
//    Boot() call, which does not return once the kernel starts.
//
// Both are read for the last time before ExitBootServices: the RAM disk by the firmware's block device and file
// system, which end with the boot services, and the initrd by the copy into the window. XNU is told neither
// address, so nothing reads them afterwards.
//
// When the device tree carries /chosen/darwinboot-loader-inputs, two empty 32-byte records, the loader writes
// into it where each input lies, which memory type describes it and what became of it. With the excluded-range
// table (MemoryWindow.h) the whole pages of a verified input that lie inside the window then count as usable
// memory, the way an operating system returns the pages of its initrd to its page allocator. An input that is
// not verified, or that a memory type with a meaning of its own describes, stays excluded.

#include "MemoryWindow.h"

#define DARWIN_INPUT_PAYLOAD_DISK  0
#define DARWIN_INPUT_INITRD        1
#define DARWIN_INPUT_COUNT         2
#define DARWIN_INPUT_RECORD_SIZE   32

// State bits of a record.
#define DARWIN_INPUT_LOCATED   1  // the range is known
#define DARWIN_INPUT_VERIFIED  2  // the range is the input: the boot volume's block device reads from it, or the caller owns it
#define DARWIN_INPUT_INSIDE    4  // the whole range lies inside the window
#define DARWIN_INPUT_USABLE    8  // its whole pages inside the window are usable memory of XNU

// The type of a range that no descriptor, or more than one memory type, describes.
#define DARWIN_INPUT_TYPE_NONE  0xFFFFFFFFU

// A flattened device tree larger than this is not read.
#define DARWIN_FDT_LIMIT  0x4000000U

// In the device tree property: base, size, type, state and usable bytes, little-endian, in this order.
typedef struct {
  UINT64  Base;
  UINT64  Size;
  UINT32  Type;
  UINT32  State;
  UINT64  Usable;
} DARWIN_LOADER_INPUT;

// The first eight data bytes of the GUID extension HOB named Guid (16 bytes, as stored) in the HOB list of
// Limit bytes at Hob. FALSE when the list ends, or leaves Limit, before such a HOB.
STATIC
BOOLEAN
DarwinHobValue (CONST UINT8 *Hob, UINTN Limit, CONST UINT8 *Guid, UINT64 *Value)
{
  UINTN   Offset;
  UINT16  Type;
  UINT16  Length;

  for (Offset = 0; (Offset <= Limit) && (Limit - Offset >= 8); Offset += Length) {
    Type = ReadUnaligned16 ((CONST UINT16 *)(Hob + Offset));
    Length = ReadUnaligned16 ((CONST UINT16 *)(Hob + Offset + 2));
    if ((Type == 0xFFFF) || (Length < 8) || (Length > Limit - Offset)) { return FALSE; }
    if ((Type == 4) && (Length >= 32) && (CompareMem (Hob + Offset + 8, Guid, 16) == 0)) {
      *Value = ReadUnaligned64 ((CONST UINT64 *)(Hob + Offset + 24));
      return TRUE;
    }
  }
  return FALSE;
}

// The bytes of a HOB list, from the end address its first HOB (the hand-off information table) records. Zero
// when the list does not begin with that table or the address does not follow it within Limit bytes.
STATIC
UINTN
DarwinHobListSize (CONST UINT8 *Hob, UINT64 Address, UINT64 Limit)
{
  UINT64  End;

  if ((ReadUnaligned16 ((CONST UINT16 *)Hob) != 1) || (ReadUnaligned16 ((CONST UINT16 *)(Hob + 2)) < 56)) { return 0; }
  End = ReadUnaligned64 ((CONST UINT64 *)(Hob + 48));
  if ((End < Address) || (End - Address < 56) || (End - Address > Limit - 8)) { return 0; }
  return (UINTN)(End - Address + 8);
}

STATIC
UINT32
DarwinBe32 (CONST UINT8 *Bytes)
{
  return ((UINT32)Bytes[0] << 24) | ((UINT32)Bytes[1] << 16) | ((UINT32)Bytes[2] << 8) | Bytes[3];
}

// TRUE when the string of the strings block that starts at Name is Wanted, terminator included.
STATIC
BOOLEAN
DarwinFdtName (CONST UINT8 *Strings, UINT32 Size, UINT32 Name, CONST CHAR8 *Wanted, UINTN WantedSize)
{
  return (Name < Size) && (WantedSize <= Size - Name) && (CompareMem (Strings + Name, Wanted, WantedSize) == 0);
}

// The range the flattened device tree at Fdt gives for the initrd: /chosen linux,initrd-start and
// linux,initrd-end, each one 32-bit or 64-bit big-endian cell value. Every offset is checked against the sizes
// in the tree's header, and nothing beyond the header's total size is read. FALSE for anything but a version 17
// tree that holds both properties once, with the end behind the start.
STATIC
BOOLEAN
DarwinFdtInitrd (CONST UINT8 *Fdt, UINT64 *Start, UINT64 *End)
{
  UINT32       Total;
  UINT32       StructOffset;
  UINT32       StructSize;
  UINT32       StringsOffset;
  UINT32       StringsSize;
  UINT32       Offset;
  UINT32       Token;
  UINT32       Length;
  UINT32       Name;
  UINT32       Depth;
  UINT64       Value;
  BOOLEAN      Chosen;
  BOOLEAN      HasStart;
  BOOLEAN      HasEnd;
  BOOLEAN      IsStart;
  BOOLEAN      IsEnd;
  CONST UINT8  *Struct;

  if (DarwinBe32 (Fdt) != 0xD00DFEED) { return FALSE; }
  Total = DarwinBe32 (Fdt + 4);
  StructOffset = DarwinBe32 (Fdt + 8);
  StringsOffset = DarwinBe32 (Fdt + 12);
  StringsSize = DarwinBe32 (Fdt + 32);
  StructSize = DarwinBe32 (Fdt + 36);
  if ((Total < 40) || (Total > DARWIN_FDT_LIMIT) || (DarwinBe32 (Fdt + 20) < 17) || (DarwinBe32 (Fdt + 24) > 17) ||
      (StructOffset < 40) || (StructOffset & 3) || (StructOffset > Total) || (StructSize > Total - StructOffset) ||
      (StringsOffset < 40) || (StringsOffset > Total) || (StringsSize > Total - StringsOffset)) { return FALSE; }
  Struct = Fdt + StructOffset;
  Depth = 0;
  Chosen = FALSE;
  HasStart = FALSE;
  HasEnd = FALSE;
  for (Offset = 0; StructSize - Offset >= 4; ) {
    Token = DarwinBe32 (Struct + Offset);
    Offset += 4;
    if (Token == 4) { continue; }
    if (Token == 1) {
      // A node: its name, terminated and padded to four bytes. /chosen is a child of the root.
      Name = Offset;
      while ((Offset < StructSize) && (Struct[Offset] != 0)) { Offset++; }
      if (Offset >= StructSize) { return FALSE; }
      Depth++;
      if ((Depth == 2) && (Offset - Name == 6) && (CompareMem (Struct + Name, "chosen", 6) == 0)) {
        if (Chosen || HasStart || HasEnd) { return FALSE; }
        Chosen = TRUE;
      }
      Offset = (Offset + 4) & ~3U;
      if (Offset > StructSize) { return FALSE; }
      continue;
    }
    if (Token == 2) {
      if (Depth == 0) { return FALSE; }
      if ((Depth == 2) && Chosen) { break; }
      Depth--;
      if (Depth == 0) { break; }
      continue;
    }
    if (Token != 3) { break; }
    if ((Depth == 0) || (StructSize - Offset < 8)) { return FALSE; }
    Length = DarwinBe32 (Struct + Offset);
    Name = DarwinBe32 (Struct + Offset + 4);
    Offset += 8;
    if ((Length > StructSize - Offset) || (((Length + 3) & ~3U) > StructSize - Offset)) { return FALSE; }
    if (Chosen && (Depth == 2)) {
      IsStart = DarwinFdtName (Fdt + StringsOffset, StringsSize, Name, "linux,initrd-start", sizeof ("linux,initrd-start"));
      IsEnd = DarwinFdtName (Fdt + StringsOffset, StringsSize, Name, "linux,initrd-end", sizeof ("linux,initrd-end"));
      if (IsStart || IsEnd) {
        if (((Length != 4) && (Length != 8)) || (IsStart ? HasStart : HasEnd)) { return FALSE; }
        Value = DarwinBe32 (Struct + Offset);
        if (Length == 8) { Value = (Value << 32) | DarwinBe32 (Struct + Offset + 4); }
        if (IsStart) { *Start = Value; HasStart = TRUE; } else { *End = Value; HasEnd = TRUE; }
      }
    }
    Offset += (Length + 3) & ~3U;
  }
  return HasStart && HasEnd && (*End > *Start);
}

// The memory type of [Start, End) when descriptors of one type cover all of it, else DARWIN_INPUT_TYPE_NONE.
STATIC
UINT32
DarwinRangeType (CONST VOID *Map, UINTN MapSize, UINTN Stride, UINT64 Start, UINT64 End)
{
  CONST EFI_MEMORY_DESCRIPTOR  *Descriptor;
  UINTN                        Offset;
  UINT64                       Cursor;
  UINT64                       Limit;
  UINT32                       Type;
  BOOLEAN                      Found;

  Type = DARWIN_INPUT_TYPE_NONE;
  if (Start >= End) { return Type; }
  for (Cursor = Start; Cursor < End; Cursor = Limit) {
    Found = FALSE;
    Limit = Cursor;
    for (Offset = 0; Offset + Stride <= MapSize; Offset += Stride) {
      Descriptor = (CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map + Offset);
      if ((Descriptor->NumberOfPages == 0) ||
          (Descriptor->NumberOfPages > (MAX_UINT64 - Descriptor->PhysicalStart) / EFI_PAGE_SIZE) ||
          (Descriptor->PhysicalStart > Cursor) ||
          (Descriptor->PhysicalStart + Descriptor->NumberOfPages * EFI_PAGE_SIZE <= Cursor)) { continue; }
      if ((Cursor != Start) && (Descriptor->Type != Type)) { return DARWIN_INPUT_TYPE_NONE; }
      Type = Descriptor->Type;
      Limit = Descriptor->PhysicalStart + Descriptor->NumberOfPages * EFI_PAGE_SIZE;
      Found = TRUE;
      break;
    }
    if (!Found) { return DARWIN_INPUT_TYPE_NONE; }
  }
  return Type;
}

// Memory types whose pages a verified input may hand to XNU: the types that are usable anyway, the pool and
// page allocations of the boot services, and memory the platform reserved for what the first-stage loader
// placed there. Every other type has a meaning of its own (runtime services, ACPI, device or unusable memory).
STATIC
BOOLEAN
DarwinInputTypeReleasable (UINT32 Type)
{
  return DarwinMemoryUsable (Type) || (Type == EfiBootServicesData) || (Type == EfiReservedMemoryType);
}

// Adds to the loader's copy of the memory map, which holds Capacity bytes, a descriptor that describes the
// page range [Start, End) as loader data. The firmware's own descriptor of the range stays; the window code
// reads usable memory from any descriptor that covers an address. FALSE when the copy has no room.
STATIC
BOOLEAN
DarwinMapAddUsable (VOID *Map, UINTN *MapSize, UINTN Capacity, UINTN Stride, UINT64 Start, UINT64 End)
{
  EFI_MEMORY_DESCRIPTOR  *Descriptor;

  if ((Stride < sizeof (EFI_MEMORY_DESCRIPTOR)) || (*MapSize > Capacity) || (Capacity - *MapSize < Stride) ||
      (Start >= End) || ((Start | End) & (EFI_PAGE_SIZE - 1))) { return FALSE; }
  Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + *MapSize);
  ZeroMem (Descriptor, Stride);
  Descriptor->Type = EfiLoaderData;
  Descriptor->PhysicalStart = Start;
  Descriptor->NumberOfPages = (End - Start) / EFI_PAGE_SIZE;
  *MapSize += Stride;
  return TRUE;
}

// Completes the records from a memory map as the firmware returned it: the type of each located input and
// whether it lies inside the window [Base, Base + Size). With Release, the whole pages of a verified input of a
// releasable type that lie inside the window become usable memory in the loader's copy of the map, so the
// excluded-range table written from the copy leaves them out. A range the copy has no room for stays excluded.
STATIC
VOID
DarwinDescribeInputs (
  VOID                 *Map,
  UINTN                *MapSize,
  UINTN                Capacity,
  UINTN                Stride,
  UINT64               Base,
  UINT64               Size,
  BOOLEAN              Release,
  DARWIN_LOADER_INPUT  *Inputs
  )
{
  UINTN   Index;
  UINT64  First;
  UINT64  Last;

  // Types first, from the firmware's descriptors alone.
  for (Index = 0; Index < DARWIN_INPUT_COUNT; Index++) {
    Inputs[Index].State &= DARWIN_INPUT_LOCATED | DARWIN_INPUT_VERIFIED;
    Inputs[Index].Usable = 0;
    Inputs[Index].Type = DARWIN_INPUT_TYPE_NONE;
    if (!(Inputs[Index].State & DARWIN_INPUT_LOCATED) || (Inputs[Index].Size == 0) ||
        (Inputs[Index].Size > MAX_UINT64 - Inputs[Index].Base)) {
      Inputs[Index].State = 0;
      continue;
    }
    Inputs[Index].Type = DarwinRangeType (Map, *MapSize, Stride, Inputs[Index].Base, Inputs[Index].Base + Inputs[Index].Size);
    if ((Inputs[Index].Base >= Base) && (Inputs[Index].Base - Base <= Size) &&
        (Inputs[Index].Size <= Size - (Inputs[Index].Base - Base))) {
      Inputs[Index].State |= DARWIN_INPUT_INSIDE;
    }
  }
  for (Index = 0; Release && (Index < DARWIN_INPUT_COUNT); Index++) {
    if (!(Inputs[Index].State & DARWIN_INPUT_VERIFIED) || !DarwinInputTypeReleasable (Inputs[Index].Type) ||
        (Inputs[Index].Base > MAX_UINT64 - (EFI_PAGE_SIZE - 1))) { continue; }
    First = (Inputs[Index].Base + EFI_PAGE_SIZE - 1) & ~(UINT64)(EFI_PAGE_SIZE - 1);
    Last = (Inputs[Index].Base + Inputs[Index].Size) & ~(UINT64)(EFI_PAGE_SIZE - 1);
    if (First < Base) { First = Base; }
    if (Last > Base + Size) { Last = Base + Size; }
    if ((First >= Last) ||
        (!DarwinMemoryUsable (Inputs[Index].Type) && !DarwinMapAddUsable (Map, MapSize, Capacity, Stride, First, Last))) { continue; }
    Inputs[Index].Usable = Last - First;
    Inputs[Index].State |= DARWIN_INPUT_USABLE;
  }
}

// Writes the records into the device tree property, which is not necessarily aligned.
STATIC
VOID
DarwinWriteInputs (UINT8 *Table, CONST DARWIN_LOADER_INPUT *Inputs)
{
  UINTN  Index;

  for (Index = 0; Index < DARWIN_INPUT_COUNT; Index++, Table += DARWIN_INPUT_RECORD_SIZE) {
    WriteUnaligned64 ((UINT64 *)Table, Inputs[Index].Base);
    WriteUnaligned64 ((UINT64 *)(Table + 8), Inputs[Index].Size);
    WriteUnaligned32 ((UINT32 *)(Table + 16), Inputs[Index].Type);
    WriteUnaligned32 ((UINT32 *)(Table + 20), Inputs[Index].State);
    WriteUnaligned64 ((UINT64 *)(Table + 24), Inputs[Index].Usable);
  }
}

#endif
