// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Guid/HobList.h>
#include <Protocol/UniXNULoader.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/RootStage.h>
EFI_STATUS DarwinResidentRun(ROOT_STAGE_TABLE *,UINT64,UINT64,UINT64,UINT64,UINT64,UINT64,EFI_MEMORY_DESCRIPTOR *,UINTN,UINTN);

#include "BootArgs.h"
#include "BootVideo.h"
#include "PackedRamdisk.h"
#include "WindowSize.h"
#include "MemoryWindow.h"
#include "LoaderInputs.h"

#define WINDOW_SIZE  SIZE_256MB
#define BLOCK_SIZE   SIZE_2MB
// The largest kernel file read: the default window. A macOS kernel collection is about 137 MiB; the window check
// below still bounds the kernel with everything placed after it.
#define KERNEL_LIMIT SIZE_256MB
#define MAX_SEGMENTS 64

typedef struct {
  UINT64  Address;
  UINT64  Size;
  UINT64  FileOffset;
  UINT64  FileSize;
  UINT32  Protection;
} SEGMENT;

typedef struct {
  SEGMENT  Segments[MAX_SEGMENTS];
  UINTN    Count;
  UINT64   Base;
  UINT64   End;
  UINT64   Entry;
} KERNEL;

// Where the parts of the loaded image go, and the bytes they take from the start of the window.


STATIC EFI_HANDLE  mImage;
UINT64 EFIAPI DarwinCurrentEl (VOID);
VOID EFIAPI DarwinEnter (UINT64 Args, UINT64 Entry, UINT64 Memory, UINT64 Size, UINT64 VideoBase, UINT64 VideoBytes);
VOID EFIAPI DarwinFlush (UINT64 Memory, UINT64 Size);

#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })

STATIC UINT32 U32 (CONST VOID *P) { return ReadUnaligned32 ((CONST UINT32 *)P); }
STATIC UINT64 U64 (CONST VOID *P) { return ReadUnaligned64 ((CONST UINT64 *)P); }

STATIC
EFI_STATUS
ReadBootVideo (UINT64 Video[6])
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL           *Gop;
  EFI_STATUS                            Status;

  ZeroMem (Video, 6 * sizeof (*Video));
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  if (Status == EFI_NOT_FOUND) { return EFI_SUCCESS; }
  if (EFI_ERROR (Status)) { return Status; }
  return DarwinBootVideoFromGop (Gop, Video);
}

STATIC
EFI_STATUS
ReadFile (
  EFI_FILE_PROTOCOL *Root,
  CONST CHAR16      *Path,
  UINTN             Limit,
  VOID              **Data,
  UINTN             *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              Actual;

  *Data = NULL;
  *Size = 0;
  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Info = NULL;
  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) { goto Done; }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) { goto Done; }
  if ((Info->Attribute & EFI_FILE_DIRECTORY) || (Info->FileSize == 0) || (Info->FileSize > Limit)) {
    Print (L"DARWINBOOT: %s attr %lx size %lu limit %lu\n", Path, (UINT64)Info->Attribute, (UINT64)Info->FileSize, (UINT64)Limit);
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  *Size = (UINTN)Info->FileSize;
  *Data = AllocatePool (*Size);
  if (*Data == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Actual = *Size;
  Status = File->Read (File, &Actual, *Data);
  if (!EFI_ERROR (Status) && (Actual != *Size)) { Status = EFI_DEVICE_ERROR; }
Done:
  if (Info != NULL) { FreePool (Info); }
  File->Close (File);
  if (EFI_ERROR (Status) && (*Data != NULL)) { FreePool (*Data); *Data = NULL; }
  return Status;
}

STATIC
EFI_STATUS
ParseKernelHeader (CONST UINT8 *Data, UINTN Size, UINTN Header, KERNEL *Kernel)
{
  UINTN    Offset;
  UINTN    End;
  UINTN    Index;
  UINTN    Other;
  UINT32   Count;
  UINT32   Command;
  UINT32   Length;
  SEGMENT  *Segment;
  BOOLEAN  HasEntry;
  BOOLEAN  Executable;

  if ((Header > Size) || (Size - Header < 32) ||
      (U32 (Data + Header) != 0xfeedfacf) || (U32 (Data + Header + 4) != 0x100000c) ||
      (((U32 (Data + Header + 8) & 0x00ffffff) != 0) &&
       ((U32 (Data + Header + 8) & 0x00ffffff) != 2)) ||
      ((U32 (Data + Header + 12) != 2) &&
       ((Header != 0) || (U32 (Data + Header + 12) != 12)))) {
    return EFI_UNSUPPORTED;
  }
  if (U32 (Data + Header + 20) > Size - Header - 32) { return EFI_LOAD_ERROR; }
  ZeroMem (Kernel, sizeof (*Kernel));
  Count = U32 (Data + Header + 16);
  End = Header + 32 + U32 (Data + Header + 20);
  Offset = Header + 32;
  HasEntry = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (End - Offset < 8) { return EFI_LOAD_ERROR; }
    Command = U32 (Data + Offset);
    Length = U32 (Data + Offset + 4);
    if ((Length < 8) || (Length & 7) || (Length > End - Offset)) { return EFI_LOAD_ERROR; }
    if (Command == 0x19) {
      if ((Length < 72) || (U32 (Data + Offset + 64) > (Length - 72) / 80) ||
          (Kernel->Count == MAX_SEGMENTS)) { return EFI_LOAD_ERROR; }
      if (U64 (Data + Offset + 32) != 0) {
        Segment = &Kernel->Segments[Kernel->Count++];
        Segment->Address = U64 (Data + Offset + 24);
        Segment->Size = U64 (Data + Offset + 32);
        Segment->FileOffset = U64 (Data + Offset + 40);
        Segment->FileSize = U64 (Data + Offset + 48);
        Segment->Protection = U32 (Data + Offset + 60);
        if ((Segment->Size > WINDOW_SIZE) || (Segment->Address > MAX_UINT64 - Segment->Size) ||
            (Segment->FileSize > Segment->Size) || (Segment->FileOffset > Size) ||
            (Segment->FileSize > Size - Segment->FileOffset)) { return EFI_LOAD_ERROR; }
      }
    } else if (Command == 5) {
      if (HasEntry || (Length != 288) || (U32 (Data + Offset + 8) != 6) ||
          (U32 (Data + Offset + 12) != 68)) { return EFI_LOAD_ERROR; }
      Kernel->Entry = U64 (Data + Offset + 272);
      HasEntry = TRUE;
    }
    Offset += Length;
  }
  if ((Offset != End) || !HasEntry || (Kernel->Count == 0)) { return EFI_LOAD_ERROR; }
  Kernel->Base = MAX_UINT64;
  Executable = FALSE;
  for (Index = 0; Index < Kernel->Count; Index++) {
    Segment = &Kernel->Segments[Index];
    Kernel->Base = MIN (Kernel->Base, Segment->Address);
    Kernel->End = MAX (Kernel->End, Segment->Address + Segment->Size);
    if ((Kernel->Entry >= Segment->Address) && (Kernel->Entry - Segment->Address < Segment->FileSize) &&
        ((Segment->Protection & 4) != 0)) { Executable = TRUE; }
    for (Other = 0; Other < Index; Other++) {
      if ((Segment->Address < Kernel->Segments[Other].Address + Kernel->Segments[Other].Size) &&
          (Kernel->Segments[Other].Address < Segment->Address + Segment->Size)) { return EFI_LOAD_ERROR; }
    }
  }
  if (!Executable || (Kernel->Entry & 3) || (Kernel->Base & EFI_PAGE_MASK) ||
      (Kernel->Base > MAX_UINT64 - WINDOW_SIZE) ||
      (Kernel->End - Kernel->Base > WINDOW_SIZE - SIZE_2MB)) { return EFI_LOAD_ERROR; }
  return EFI_SUCCESS;
}

// Fileset segment offsets are absolute in the collection. XNU rebases and
// signs its own fixup chains after entry; firmware only copies root mappings.
STATIC
EFI_STATUS
ParseKernel (CONST UINT8 *Data, UINTN Size, KERNEL *Kernel)
{
  EFI_STATUS Status;
  KERNEL     Member;
  UINTN      Offset;
  UINTN      End;
  UINTN      Header;
  UINTN      Name;
  UINTN      Index;
  UINTN      Other;
  UINT32     Length;
  BOOLEAN    Found;
  BOOLEAN    Covered;
  SEGMENT    *Child;
  SEGMENT    *Parent;

  Status = ParseKernelHeader (Data, Size, 0, Kernel);
  if (EFI_ERROR (Status) || (U32 (Data + 12) != 12)) { return Status; }
  Offset = 32;
  End = 32 + U32 (Data + 20);
  Found = FALSE;
  while (Offset < End) {
    Length = U32 (Data + Offset + 4);
    if (U32 (Data + Offset) == 0x80000035) {
      if (Length < 40) { return EFI_LOAD_ERROR; }
      Name = U32 (Data + Offset + 24);
      if ((Name < 32) || (Name >= Length)) { return EFI_LOAD_ERROR; }
      for (Index = Name; (Index < Length) && (Data[Offset + Index] != 0); Index++) {}
      if (Index == Length) { return EFI_LOAD_ERROR; }
      Header = U64 (Data + Offset + 16);
      if ((Header > Size) || (Size - Header < 32) || (Header & 7)) { return EFI_LOAD_ERROR; }
      // Every indexed header must actually be loaded at its advertised VA.
      Covered = FALSE;
      for (Other = 0; Other < Kernel->Count; Other++) {
        Parent = &Kernel->Segments[Other];
        if ((Header >= Parent->FileOffset) &&
            (Header - Parent->FileOffset <= Parent->FileSize) &&
            (32 <= Parent->FileSize - (Header - Parent->FileOffset)) &&
            (U64 (Data + Offset + 8) == Parent->Address + Header - Parent->FileOffset)) {
          Covered = TRUE;
        }
      }
      if (!Covered) { return EFI_LOAD_ERROR; }
      if ((Index - Name == sizeof ("com.apple.kernel") - 1) &&
          (CompareMem (Data + Offset + Name, "com.apple.kernel", sizeof ("com.apple.kernel")) == 0)) {
        if (Found || (U32 (Data + Header + 12) != 2) ||
            ((U32 (Data + Header + 8) & 0x00ffffff) != (U32 (Data + 8) & 0x00ffffff)) ||
            ((U32 (Data + Header + 24) & 0x80000000) == 0)) { return EFI_LOAD_ERROR; }
        Found = TRUE;
        Status = ParseKernelHeader (Data, Size, Header, &Member);
        if (EFI_ERROR (Status) || (Member.Entry != Kernel->Entry)) { return EFI_LOAD_ERROR; }
        for (Index = 0; Index < Member.Count; Index++) {
          Child = &Member.Segments[Index];
          Covered = FALSE;
          for (Other = 0; Other < Kernel->Count; Other++) {
            Parent = &Kernel->Segments[Other];
            if ((Child->Address >= Parent->Address) &&
                (Child->Address - Parent->Address <= Parent->Size) &&
                (Child->Size <= Parent->Size - (Child->Address - Parent->Address)) &&
                ((Child->Protection & ~Parent->Protection) == 0) &&
                ((Child->FileSize == 0) ||
                 ((Child->Address - Parent->Address <= Parent->FileSize) &&
                  (Child->FileSize <= Parent->FileSize - (Child->Address - Parent->Address)) &&
                  (Child->FileOffset == Parent->FileOffset + Child->Address - Parent->Address)))) {
              Covered = TRUE;
            }
          }
          if (!Covered) { return EFI_LOAD_ERROR; }
        }
      }
    }
    Offset += Length;
  }
  return Found ? EFI_SUCCESS : EFI_LOAD_ERROR;
}

STATIC
BOOLEAN
AdtNode (CONST UINT8 *Data, UINTN Size, UINTN *Offset, UINTN Depth, UINTN ParentPath, UINTN *RamdiskOffset, UINTN *TrustCacheOffset, UINTN *AddonOffset, UINTN *PolicyOffset, UINTN *RootOffset, UINT64 *WindowSize, BOOLEAN *KeepPacked, UINTN *ExcludedOffset, UINTN *ExcludedCapacity, UINTN *InputsOffset)
{
  UINT32 Properties;
  UINT32 Children;
  UINT32 Length;
  UINTN  Index;
  UINTN  Path;
  UINTN  Candidate;
  UINTN  TrustCandidate;
  UINTN  AddonCandidate;
  UINTN  PolicyCandidate;
  UINTN  MemoryCandidate;
  UINTN  RootCandidate;
  UINTN  ExcludedCandidate;
  UINTN  ExcludedLength;
  UINTN  InputsCandidate;
  CONST UINT8 *Name;
  UINTN  NameLength;

  Name = NULL;
  NameLength = 0;
  Candidate = MAX_UINTN;
  TrustCandidate = MAX_UINTN;
  AddonCandidate = MAX_UINTN;
  PolicyCandidate = MAX_UINTN;
  MemoryCandidate = MAX_UINTN;
  RootCandidate = MAX_UINTN;
  ExcludedCandidate = MAX_UINTN;
  ExcludedLength = 0;
  InputsCandidate = MAX_UINTN;
  if ((Depth > 64) || (Size - *Offset < 8)) { return FALSE; }
  Properties = U32 (Data + *Offset);
  Children = U32 (Data + *Offset + 4);
  *Offset += 8;
  if (Properties > (Size - *Offset) / 36) { return FALSE; }
  for (Index = 0; Index < Properties; Index++) {
    if (Size - *Offset < 36) { return FALSE; }
    if (ScanMem8 (Data + *Offset, 32, 0) == NULL) { return FALSE; }
    Length = U32 (Data + *Offset + 32);
    *Offset += 36;
    if (Length > Size - *Offset) { return FALSE; }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "name") == 0) {
      if (Name != NULL) { return FALSE; }
      Name = Data + *Offset;
      NameLength = Length;
    }
    if ((AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RAMDisk") == 0) ||
        (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0)) {
      if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0) {
        if (*KeepPacked) { return FALSE; }
        *KeepPacked = TRUE;
      }
      if ((Candidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      Candidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "TrustCache") == 0) {
      if ((TrustCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      TrustCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "GpuAddon") == 0) {
      if ((AddonCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      AddonCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RootPolicies") == 0) {
      if ((PolicyCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      PolicyCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RootDisks") == 0) {
      if ((RootCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      RootCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-size") == 0) {
      if ((MemoryCandidate != MAX_UINTN) || (Length != 8)) { return FALSE; }
      MemoryCandidate = *Offset;
    }
    // The table the loader fills with the ranges of the window XNU must not use (MemoryWindow.h): empty pairs.
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-excluded") == 0) {
      if ((ExcludedCandidate != MAX_UINTN) || (Length < 16) || (Length & 15) ||
          !IsZeroBuffer (Data + *Offset, Length)) { return FALSE; }
      ExcludedCandidate = *Offset;
      ExcludedLength = Length;
    }
    // The records the loader fills with the memory of its inputs (LoaderInputs.h): empty.
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-loader-inputs") == 0) {
      if ((InputsCandidate != MAX_UINTN) || (Length != DARWIN_INPUT_COUNT * DARWIN_INPUT_RECORD_SIZE) ||
          !IsZeroBuffer (Data + *Offset, Length)) { return FALSE; }
      InputsCandidate = *Offset;
    }
    *Offset += Length;
    if (ALIGN_VALUE (*Offset, 4) > Size) { return FALSE; }
    *Offset = ALIGN_VALUE (*Offset, 4);
  }
  Path = 3;
  if (Depth == 0) { Path = 0; }
  if ((Depth == 1) && (ParentPath == 0) && (NameLength == 7) && (CompareMem (Name, "chosen", 7) == 0)) { Path = 1; }
  if ((Depth == 2) && (ParentPath == 1) && (NameLength == 11) && (CompareMem (Name, "memory-map", 11) == 0)) { Path = 2; }
  if ((Path == 2) && (Candidate != MAX_UINTN)) {
    if (*RamdiskOffset != MAX_UINTN) { return FALSE; }
    *RamdiskOffset = Candidate;
  }
  if ((Path == 2) && (TrustCandidate != MAX_UINTN)) {
    if (*TrustCacheOffset != MAX_UINTN) { return FALSE; }
    *TrustCacheOffset = TrustCandidate;
  }
  if ((Path == 2) && (AddonCandidate != MAX_UINTN)) {
    if (*AddonOffset != MAX_UINTN) { return FALSE; }
    *AddonOffset = AddonCandidate;
  }
  if (PolicyCandidate != MAX_UINTN) {
    if ((Path != 2) || (*PolicyOffset != MAX_UINTN)) { return FALSE; }
    *PolicyOffset = PolicyCandidate;
  }
  if (MemoryCandidate != MAX_UINTN) {
    if ((Path != 1) || (*WindowSize != 0)) { return FALSE; }
    *WindowSize = U64 (Data + MemoryCandidate);
    if (!DarwinWindowSizeValid (*WindowSize)) { return FALSE; }
  }
  if ((Path == 2) && (RootCandidate != MAX_UINTN)) {
    if (*RootOffset != MAX_UINTN) { return FALSE; }
    *RootOffset = RootCandidate;
  }
  if (ExcludedCandidate != MAX_UINTN) {
    if ((Path != 1) || (*ExcludedOffset != MAX_UINTN)) { return FALSE; }
    *ExcludedOffset = ExcludedCandidate;
    *ExcludedCapacity = ExcludedLength / 16;
  }
  if (InputsCandidate != MAX_UINTN) {
    if ((Path != 1) || (*InputsOffset != MAX_UINTN)) { return FALSE; }
    *InputsOffset = InputsCandidate;
  }
  if (Children > (Size - *Offset) / 8) { return FALSE; }
  for (Index = 0; Index < Children; Index++) {
    if (!AdtNode (Data, Size, Offset, Depth + 1, Path, RamdiskOffset, TrustCacheOffset, AddonOffset, PolicyOffset, RootOffset, WindowSize, KeepPacked, ExcludedOffset, ExcludedCapacity, InputsOffset)) { return FALSE; }
  }
  return TRUE;
}

STATIC
BOOLEAN
ValidTrustCaches (CONST UINT8 *Data, UINTN Size)
{
  UINTN Count;
  UINTN Index;
  UINTN Start;
  UINTN End;
  UINTN Entries;
  UINTN Entry;

  if (Size < 8) { return FALSE; }
  Count = U32 (Data);
  if ((Count == 0) || (Count > 128) || (Count > (Size - 4) / 4)) { return FALSE; }
  for (Index = 0; Index < Count; Index++) {
    Start = U32 (Data + 4 + Index * 4);
    End = (Index + 1 < Count) ? U32 (Data + 8 + Index * 4) : Size;
    if ((Start < 4 + Count * 4) || (Start & 3) || (End > Size) ||
        (End < Start) || (End - Start < 24) || (U32 (Data + Start) != 2)) { return FALSE; }
    Entries = U32 (Data + Start + 20);
    if ((Entries > (End - Start - 24) / 24) || (End - Start != 24 + Entries * 24)) { return FALSE; }
    for (Entry = 0; Entry < Entries; Entry++) {
      CONST UINT8 *Hash = Data + Start + 24 + Entry * 24;
      if ((Hash[20] < 1) || (Hash[20] > 4) ||
          ((Entry != 0) && (CompareMem (Hash - 24, Hash, 20) >= 0))) { return FALSE; }
    }
  }
  return TRUE;
}

STATIC
EFI_STATUS
GetMap (EFI_MEMORY_DESCRIPTOR **Map, UINTN *Capacity, UINTN *Size, UINTN *Key, UINTN *Stride)
{
  EFI_STATUS Status;
  UINT32     Version;
  UINTN      Attempt;

  for (Attempt = 0; Attempt < 3; Attempt++) {
    *Size = *Capacity;
    Status = gBS->GetMemoryMap (Size, *Map, Key, Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) { return Status; }
    if (*Map != NULL) { FreePool (*Map); }
    *Capacity = *Size + 32 * sizeof (EFI_MEMORY_DESCRIPTOR);
    *Map = AllocatePool (*Capacity);
    if (*Map == NULL) { return EFI_OUT_OF_RESOURCES; }
  }
  return EFI_OUT_OF_RESOURCES;
}

// End of the run of EfiConventionalMemory that begins at Start: the UEFI map often lists physically
// adjacent free ranges as separate descriptors, so extend over every descriptor that continues the run.
STATIC
UINT64
RunEnd (EFI_MEMORY_DESCRIPTOR *Map, UINTN Size, UINTN Stride, UINT64 Start, UINT64 End)
{
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  UINTN                 Offset;
  BOOLEAN               Extended;

  do {
    Extended = FALSE;
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->PhysicalStart == End) &&
          (Descriptor->NumberOfPages != 0) && (Descriptor->NumberOfPages < (MAX_UINT64 - End) / EFI_PAGE_SIZE)) {
        End += EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
        Extended = TRUE;
      }
    }
  } while (Extended);
  (VOID)Start;
  return End;
}

// Large diagnostic text straight into the GOP framebuffer: the UEFI text console font is too small to
// photograph. 5x12 glyphs from DiagFont.h scaled 4x, white on a black box; wraps at the right edge.
#include "DiagFont.h"
STATIC CONST UINT64 mDiagFont[] = GLYPH_FONT;

STATIC
VOID
DiagDraw (CONST CHAR16 *Text, UINTN Row)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  UINT64                       Video[6];
  UINTN                        Scale = 4, X = 0, Y, Index, Gx, Gy, Dx, Dy, Cell, Line;
  UINT32                       *Fb;
  UINT64                       Glyph, Data;
  CHAR16                       Ch;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) ||
      EFI_ERROR (DarwinBootVideoFromGop (Gop, Video))) { return; }
  Cell = 6 * Scale;
  Line = 14 * Scale;
  Y = Video[4] / 4 + Row * Line;
  Fb = (UINT32 *)(UINTN)Video[0];
  for (Index = 0; Text[Index] != 0; Index++) {
    Ch = Text[Index];
    if ((Ch < 32) || (Ch > 126)) { continue; }
    if (X + Cell > Video[3]) { X = 0; Y += Line; }
    if (Y + Line > Video[4]) { return; }
    Glyph = mDiagFont[Ch - 32];
    for (Gy = 0; Gy < 14 * Scale; Gy++) {
      for (Gx = 0; Gx < Cell; Gx++) {
        Fb[(Y + Gy) * (Video[2] / 4) + X + Gx] = 0xFF000000;
      }
    }
    for (Gy = 0; Gy < 12; Gy++) {
      Data = (Gy < 6) ? ((Glyph >> 32) >> (Gy * 5)) : ((Glyph & 0xFFFFFFFFULL) >> ((Gy - 6) * 5));
      for (Gx = 0; Gx < 5; Gx++) {
        if ((Data >> Gx) & 1) {
          for (Dy = 0; Dy < Scale; Dy++) {
            for (Dx = 0; Dx < Scale; Dx++) {
              Fb[(Y + Gy * Scale + Dy) * (Video[2] / 4) + X + Gx * Scale + Dx] = 0xFFFFFFFF;
            }
          }
        }
      }
    }
    X += Cell;
  }
}

STATIC
EFI_STATUS
AllocateWindow (KERNEL *Kernel, UINT64 WindowSize, EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  EFI_STATUS            Status;
  UINTN                 Capacity;
  UINTN                 Size;
  UINTN                 Key;
  UINTN                 Stride;
  UINTN                 Offset;
  UINT64                End;
  UINT64                Candidate;
  UINT64                BestStart;
  UINT64                BestBytes;
  UINT64                RunBytes;

  *Address = 0;
  Map = NULL;
  Capacity = 0;
  BestStart = 0;
  BestBytes = 0;
  Status = GetMap (&Map, &Capacity, &Size, &Key, &Stride);
  if (EFI_ERROR (Status)) { if (Map != NULL) { FreePool (Map); } return Status; }
  Status = EFI_OUT_OF_RESOURCES;
  for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    if ((Descriptor->Type != EfiConventionalMemory) || (Descriptor->NumberOfPages == 0)) { continue; }
    End = RunEnd (Map, Size, Stride, Descriptor->PhysicalStart,
                  Descriptor->PhysicalStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
    RunBytes = End - Descriptor->PhysicalStart;
    if (RunBytes > BestBytes) { BestBytes = RunBytes; BestStart = Descriptor->PhysicalStart; }
    Candidate = ALIGN_VALUE (Descriptor->PhysicalStart, BLOCK_SIZE) + (Kernel->Base & (BLOCK_SIZE - 1));
    if ((Candidate < Descriptor->PhysicalStart) || (End < Candidate) || (End - Candidate < WindowSize)) { continue; }
    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);
    if (!EFI_ERROR (Status)) { *Address = Candidate; break; }
  }
  if (EFI_ERROR (Status)) {
    CHAR16 Line[96];
    UINTN  Row = 1;
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx BIG RUN %lx @ %lx", WindowSize, BestBytes, BestStart);
    DiagDraw (Line, 0);
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->NumberOfPages >= 0x400) && (Row < 8)) {
        UnicodeSPrint (Line, sizeof (Line), L"FREE %lx + %lx", Descriptor->PhysicalStart, EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
        DiagDraw (Line, Row++);
      }
    }

  }
  FreePool (Map);
  return Status;
}

// The window of a kernel that accepts excluded ranges (MemoryWindow.h): the start with the most usable memory
// inside WindowSize bytes. Only the head is allocated. The rest of the window is whatever the final memory map
// leaves usable, so the firmware keeps the memory it needs until ExitBootServices.
STATIC
EFI_STATUS
AllocateSpan (KERNEL *Kernel, UINT64 WindowSize, UINT64 HeadSize, EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_STATUS            Status;
  UINTN                 Capacity;
  UINTN                 Size;
  UINTN                 Key;
  UINTN                 Stride;
  UINT64                Candidate;
  UINT64                Usable;
  UINT64                SkipUsable;
  UINT64                SkipAddress;
  CHAR16                Line[96];

  *Address = 0;
  Map = NULL;
  Capacity = 0;
  Status = GetMap (&Map, &Capacity, &Size, &Key, &Stride);
  if (EFI_ERROR (Status)) { if (Map != NULL) { FreePool (Map); } return Status; }
  SkipUsable = MAX_UINT64;
  SkipAddress = 0;
  for (;;) {
    Candidate = DarwinWindowStart (Map, Size, Stride, BLOCK_SIZE, Kernel->Base & (BLOCK_SIZE - 1), HeadSize, WindowSize,
                                   SkipUsable, SkipAddress, &Usable);
    if (Candidate == 0) { Status = EFI_OUT_OF_RESOURCES; break; }
    SkipUsable = Usable;
    SkipAddress = Candidate;
    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (HeadSize), &Candidate);
    if (!EFI_ERROR (Status)) { *Address = Candidate; break; }
  }
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx HEAD %lx", WindowSize, HeadSize);
    DiagDraw (Line, 0);
  }
  FreePool (Map);
  return Status;
}


// The table for free memory outside the window (MemoryWindow.h DarwinOutsideRanges): the property
// darwinboot-memory-outside of the flattened tree, whose value is empty (base, size) pairs. A property record is a
// 32-byte name, a 32-bit length and the value. FALSE when the tree has none or its value is not such a table.
STATIC
BOOLEAN
FindOutsideTable (CONST UINT8 *Tree, UINTN TreeSize, UINTN *TableOffset, UINTN *Capacity)
{
  STATIC CONST CHAR8  Name[32] = "darwinboot-memory-outside";
  UINTN               Offset;
  UINT32              Length;

  for (Offset = 0; Offset + 36 <= TreeSize; Offset += 4) {
    if (CompareMem (Tree + Offset, Name, sizeof (Name)) != 0) { continue; }
    Length = ReadUnaligned32 ((CONST UINT32 *)(Tree + Offset + 32)) & 0x7fffffff;
    if ((Length < 16) || (Length & 15) || (Length > TreeSize - Offset - 36) ||
        !IsZeroBuffer (Tree + Offset + 36, Length)) { return FALSE; }
    *TableOffset = Offset + 36;
    *Capacity = Length / 16;
    return TRUE;
  }
  return FALSE;
}

// Fill the device tree's table from a memory map and report the usable bytes as the memory size. FALSE when the
// head is no longer whole, which the head allocation rules out.
STATIC
BOOLEAN
DescribeWindow (
  CONST VOID        *Map,
  UINTN             MapSize,
  UINTN             Stride,
  UINT64            Address,
  UINT64            WindowSize,
  UINT64            HeadSize,
  UINT8             *Table,
  UINTN             Capacity,
  DARWIN_BOOT_ARGS  *Args,
  UINTN             *Count
  )
{
  UINT64 Excluded;

  if (DarwinRunEnd (Map, MapSize, Stride, FALSE, Address, Address + HeadSize) != Address + HeadSize) { return FALSE; }
  *Count = DarwinWindowExclusions (Map, MapSize, Stride, Address, WindowSize, Table, Capacity, &Excluded);
  Args->MemorySizeActual = WindowSize - Excluded;
  return TRUE;
}

// The entry argument the firmware's SEC phase keeps from the first-stage boot loader: the address of that
// loader's device tree, as a 64-bit value in this GUID extension HOB.
STATIC CONST EFI_GUID mEntryArgumentHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

// Where the loader's inputs lie (LoaderInputs.h). The initrd is the caller's buffer. The payload RAM disk is
// the range the first-stage loader's device tree names, rounded up to whole sectors as the firmware's block
// device publishes it; it counts as verified when the boot volume's block device has exactly that size and
// reads its first and last sector from that memory, which is touched only once the size matches.
STATIC
VOID
LocateInputs (CONST UNIXNU_LOADER_BOOT_REQUEST *Request, DARWIN_LOADER_INPUT *Inputs)
{
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  VOID                   *Hob;
  UINTN                  HobSize;
  UINT64                 Tree;
  UINT64                 Start;
  UINT64                 End;
  UINT64                 Size;
  UINT8                  Sector[512];

  ZeroMem (Inputs, DARWIN_INPUT_COUNT * sizeof (*Inputs));
  if ((Request->Ramdisk != NULL) && (Request->RamdiskSize != 0)) {
    Inputs[DARWIN_INPUT_INITRD].Base = (UINT64)(UINTN)Request->Ramdisk;
    Inputs[DARWIN_INPUT_INITRD].Size = Request->RamdiskSize;
    Inputs[DARWIN_INPUT_INITRD].State = DARWIN_INPUT_LOCATED | DARWIN_INPUT_VERIFIED;
  }
  Hob = NULL;
  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiHobListGuid, &Hob)) || (Hob == NULL)) { return; }
  HobSize = DarwinHobListSize (Hob, (UINT64)(UINTN)Hob, SIZE_16MB);
  if ((HobSize == 0) || !DarwinHobValue (Hob, HobSize, (CONST UINT8 *)&mEntryArgumentHobGuid, &Tree) ||
      (Tree == 0) || (Tree & 3) || !DarwinFdtInitrd ((CONST UINT8 *)(UINTN)Tree, &Start, &End)) { return; }
  Size = End - Start;
  if ((Size > MAX_UINT64 - 511) || (ALIGN_VALUE (Size, 512) > MAX_UINT64 - Start)) { return; }
  Size = ALIGN_VALUE (Size, 512);
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].Base = Start;
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].Size = Size;
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].State = DARWIN_INPUT_LOCATED;
  if (EFI_ERROR (gBS->HandleProtocol (Request->Volume, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
      (BlockIo->Media == NULL) || BlockIo->Media->LogicalPartition || !BlockIo->Media->MediaPresent ||
      (BlockIo->Media->BlockSize != sizeof (Sector)) || (BlockIo->Media->LastBlock != Size / sizeof (Sector) - 1)) { return; }
  if (EFI_ERROR (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, sizeof (Sector), Sector)) ||
      (CompareMem (Sector, (CONST VOID *)(UINTN)Start, sizeof (Sector)) != 0) ||
      EFI_ERROR (BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, BlockIo->Media->LastBlock, sizeof (Sector), Sector)) ||
      (CompareMem (Sector, (CONST VOID *)(UINTN)(Start + Size - sizeof (Sector)), sizeof (Sector)) != 0)) { return; }
  Inputs[DARWIN_INPUT_PAYLOAD_DISK].State |= DARWIN_INPUT_VERIFIED;
}

STATIC
EFI_STATUS
EFIAPI
Boot (
  IN UNIXNU_LOADER_PROTOCOL    *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST *Request
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL               *Root;
  VOID                            *Image;
  VOID                            *Tree;
  VOID                            *TrustCaches;
  VOID                            *Addon;
  UINTN                           ImageSize;
  UINTN                           TreeSize;
  UINTN                           TrustCacheSize;
  UINTN                           AddonSize;
  CHAR16                          *TreePath;
  UINTN                           Length;
  UINTN                           Index;
  UINTN                           Offset;
  KERNEL                          Kernel;
  EFI_PHYSICAL_ADDRESS            Address;
  UINT64                          ArgsAddress;
  UINT64                          TreeAddress;
  UINT64                          Used;
  UINT64                          WindowSize;
  UINT64                          RamdiskAddress;
  UINTN                           RamdiskOffset;
  UINTN                           RamdiskBytes;
  UINTN                           RamdiskExpandedSize;
  BOOLEAN                         RamdiskPacked;
  BOOLEAN                         KeepPacked;
  UINTN                           TrustCacheOffset;
  UINT64                          TrustCacheAddress;
  UINTN                           TrustCacheMemorySize;
  UINTN                           Prefix;
  UINT64                          KernelAddress;
  UINTN                           AddonOffset;
  UINT64                          AddonAddress;
  VOID                            *Policies;
  UINTN                           PolicySize;
  UINTN                           PolicyOffset;
  UINT64                          PolicyAddress;
  UINTN                           RootOffset;
  ROOT_STAGE_TABLE                *RootStage;
  DARWIN_BOOT_ARGS                *Args;
  EFI_MEMORY_DESCRIPTOR           *Map;
  UINTN                           Capacity;
  UINTN                           MapSize;
  UINTN                           Key;
  UINTN                           Stride;
  UINT32                          Version;
  UINTN                           Attempt;
  CHAR8                           CommandLine[1024];
  UINT64                          Video[6];
  UINTN                           ExcludedOffset;
  UINTN                           ExcludedCapacity;
  UINTN                           ExcludedCount;
  UINT8                           *Excluded;
  UINT64                          HeadSize;
  UINT64                          Allocated;
  UINT64                          Cursor;
  UINTN                           InputsOffset;
  UINTN                           OutsideOffset;
  UINTN                           OutsideCapacity;
  UINTN                           OutsideCount;
  UINT64                          OutsideBytes;
  DARWIN_LOADER_INPUT             Inputs[DARWIN_INPUT_COUNT];

  (VOID)This;
  if ((Request == NULL) || (Request->Volume == NULL) || (Request->KernelPath == NULL)) { return EFI_INVALID_PARAMETER; }
  if ((Request->KextCount != 0) || (Request->DeviceTree != NULL) || (Request->DeviceTreeSize != 0) ||
      (DarwinCurrentEl () != 4)) {
    Print (L"DARWINBOOT: unsupported inputs or exception level; firmware retained\n");
    return EFI_UNSUPPORTED;
  }
  if (!RamdiskLayout (Request->Ramdisk, Request->RamdiskSize, &RamdiskExpandedSize, &RamdiskPacked)) {
    Print (L"DARWINBOOT: ramdisk layout rejected, %lu bytes\n", (UINT64)Request->RamdiskSize);
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (CommandLine, sizeof (CommandLine));
  if (Request->BootArgs != NULL) {
    for (Index = 0; Request->BootArgs[Index] != 0; Index++) {
      if ((Index >= sizeof (CommandLine) - 1) || (Request->BootArgs[Index] > 0x7f)) {
        Print (L"DARWINBOOT: boot args rejected at %lu\n", (UINT64)Index);
        return EFI_BAD_BUFFER_SIZE;
      }
      CommandLine[Index] = (CHAR8)Request->BootArgs[Index];
    }
  }
Reload:
  Root = NULL;
  Image = NULL;
  Tree = NULL;
  TrustCaches = NULL;
  Addon = NULL;
  AddonSize = 0;
  Policies = NULL;
  PolicySize = 0;
  TreePath = NULL;
  Map = NULL;
  Address = 0;
  Allocated = 0;
  HeadSize = 0;
  Excluded = NULL;
  ExcludedCount = 0;
  RootStage = NULL;
  {
    EFI_GUID Guid=ROOT_STAGE_GUID;
    for (UINTN I=0;I<gST->NumberOfTableEntries;I++)
      if (CompareGuid(&gST->ConfigurationTable[I].VendorGuid,&Guid))
        RootStage=gST->ConfigurationTable[I].VendorTable;
    if (RootStage && (RootStage->Magic!=ROOT_STAGE_MAGIC || RootStage->Version!=ROOT_STAGE_VERSION)) return EFI_INCOMPATIBLE_VERSION;
  }
  Capacity = 0;
  Status = gBS->HandleProtocol (Request->Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) { return Status; }
  if (RootStage && RootStage->Disks[2].Bytes) {
    ROOT_STAGE_DISK *Update=&RootStage->Disks[2];
    if (Update->Received!=Update->Bytes || Update->Count!=1) { Status=EFI_NOT_READY; goto Fail; }
    ImageSize=Update->Bytes;
    Image=AllocateCopyPool(ImageSize,(VOID *)(UINTN)Update->Extents[0].Address);
    Status=Image ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
  } else Status = ReadFile (Root, Request->KernelPath, KERNEL_LIMIT, &Image, &ImageSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  Status = ParseKernel (Image, ImageSize, &Kernel);
  if (EFI_ERROR (Status)) { goto Fail; }
  Length = StrSize (Request->KernelPath);
  TreePath = AllocatePool (Length + 11 * sizeof (CHAR16));
  if (TreePath == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".adt");
  Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  Offset = 0;
  RamdiskOffset = MAX_UINTN;
  TrustCacheOffset = MAX_UINTN;
  AddonOffset = MAX_UINTN;
  RootOffset = MAX_UINTN;
  PolicyOffset = MAX_UINTN;
  WindowSize = 0;
  KeepPacked = FALSE;
  ExcludedOffset = MAX_UINTN;
  ExcludedCapacity = 0;
  InputsOffset = MAX_UINTN;
  OutsideOffset = MAX_UINTN;
  OutsideCapacity = 0;
  if (!FindOutsideTable (Tree, TreeSize, &OutsideOffset, &OutsideCapacity)) { OutsideOffset = MAX_UINTN; }
  if (!AdtNode (Tree, TreeSize, &Offset, 0, 3, &RamdiskOffset, &TrustCacheOffset, &AddonOffset, &PolicyOffset, &RootOffset, &WindowSize, &KeepPacked, &ExcludedOffset, &ExcludedCapacity, &InputsOffset) || (Offset != TreeSize)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (KeepPacked && (!RamdiskPacked || (RamdiskOffset == MAX_UINTN))) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (WindowSize == 0) { WindowSize = WINDOW_SIZE; }
  if (Kernel.Base > MAX_UINT64 - WindowSize) { Status = EFI_LOAD_ERROR; goto Fail; }
  if (Request->Ramdisk != NULL) {
    if ((RamdiskOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + RamdiskOffset) != 0) ||
        (U64 ((UINT8 *)Tree + RamdiskOffset + 8) != 0)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  } else if (RamdiskOffset != MAX_UINTN) {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".trustcache");
  Status = ReadFile (Root, TreePath, SIZE_16MB, &TrustCaches, &TrustCacheSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (TrustCaches != NULL) {
    if ((TrustCacheOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + TrustCacheOffset) != 0) ||
        (U64 ((UINT8 *)Tree + TrustCacheOffset + 8) != 0) || !ValidTrustCaches (TrustCaches, TrustCacheSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (TrustCacheOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  //
  // The GPU addon (kernel.macho.gpuaddon, scripts/make-gpu-addon.py): files the kernel writes into the Data volume before its addon
  // mounts. Only its header is checked here; the kernel validates every entry.
  //
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".gpuaddon");
  Status = ReadFile (Root, TreePath, SIZE_64MB, &Addon, &AddonSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (Addon != NULL) {
    if ((AddonOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + AddonOffset) != 0) || (U64 ((UINT8 *)Tree + AddonOffset + 8) != 0) ||
        (AddonSize < 24) || (CompareMem (Addon, "NGPUADD2", 8) != 0) || (U64 ((UINT8 *)Addon + 16) != AddonSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (AddonOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  // Root hash tables live outside the kernel's coalesced constant segments.
  // The kernel authenticates each envelope against its compiled owner anchor.
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".vroot");
  Status = ReadFile (Root, TreePath, SIZE_128MB, &Policies, &PolicySize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (Policies != NULL) {
    if ((PolicyOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + PolicyOffset) != 0) ||
        (U64 ((UINT8 *)Tree + PolicyOffset + 8) != 0) || (PolicySize < 32) ||
        (CompareMem (Policies, "VROOTB01", 8) != 0) || (U64 ((UINT8 *)Policies + 8) != 1) ||
        (U64 ((UINT8 *)Policies + 16) == 0) || (U64 ((UINT8 *)Policies + 16) > 16) ||
        (U64 ((UINT8 *)Policies + 24) != PolicySize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (PolicyOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  Status = ReadBootVideo (Video);
  if (EFI_ERROR (Status)) { goto Fail; }
  if (RootOffset != MAX_UINTN) {
    EFI_GUID RootGuid = ROOT_STAGE_GUID;
    for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
      if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &RootGuid)) {
        RootStage = gST->ConfigurationTable[Index].VendorTable;
        break;
      }
    }
    if ((RootStage == NULL) || (RootStage->Magic != ROOT_STAGE_MAGIC) ||
        (RootStage->Version != ROOT_STAGE_VERSION) || (RootStage->KernelBytes != WindowSize) ||
        (RootStage->KernelBase == 0) || (RootStage->KernelBase & (BLOCK_SIZE - 1))) {
      Status = EFI_COMPROMISED_DATA; goto Fail;
    }
    if (ExcludedOffset != MAX_UINTN) {
      // The root stage already owns the whole window; an excluded-range head has no owner.
      Status = EFI_UNSUPPORTED; goto Fail;
    }
    Address = RootStage->KernelBase + (Kernel.Base & (BLOCK_SIZE - 1));
    Allocated = 0;
  }
  TrustCacheMemorySize = ALIGN_VALUE (TrustCacheSize, SIZE_16KB);
  Prefix = ALIGN_VALUE (TrustCacheMemorySize, BLOCK_SIZE);
  RamdiskBytes = ALIGN_VALUE (KeepPacked ? Request->RamdiskSize : RamdiskExpandedSize, EFI_PAGE_SIZE);
  if (RootOffset == MAX_UINTN) {
    if (ExcludedOffset == MAX_UINTN) {
      Status = AllocateWindow (&Kernel, WindowSize, &Address);
      Allocated = WindowSize;
    } else {
      // Measure the image at a stand-in head; every boundary is at most 16 KiB
      // aligned, so the byte count is the same wherever the window lands.
      KernelAddress = BLOCK_SIZE + (Kernel.Base & (BLOCK_SIZE - 1)) + Prefix;
      ArgsAddress = ALIGN_VALUE (KernelAddress + Kernel.End - Kernel.Base, SIZE_16KB);
      TreeAddress = ArgsAddress + EFI_PAGE_SIZE;
      RamdiskAddress = ALIGN_VALUE (TreeAddress + TreeSize, EFI_PAGE_SIZE);
      AddonAddress = ALIGN_VALUE (RamdiskAddress + RamdiskBytes, SIZE_16KB);
      PolicyAddress = ALIGN_VALUE (AddonAddress + AddonSize, SIZE_16KB);
      Used = ALIGN_VALUE (PolicyAddress + PolicySize, SIZE_16KB) - BLOCK_SIZE - (Kernel.Base & (BLOCK_SIZE - 1));
      HeadSize = ALIGN_VALUE (Used + DARWIN_WINDOW_RESERVE (WindowSize), EFI_PAGE_SIZE);
      Status = AllocateSpan (&Kernel, WindowSize, HeadSize, &Address);
      Allocated = HeadSize;
    }
    if (EFI_ERROR (Status)) { goto Fail; }
  }
  KernelAddress = Address + Prefix;
  ArgsAddress = ALIGN_VALUE (KernelAddress + Kernel.End - Kernel.Base, SIZE_16KB);
  TreeAddress = ArgsAddress + EFI_PAGE_SIZE;
  RamdiskAddress = ALIGN_VALUE (TreeAddress + TreeSize, EFI_PAGE_SIZE);
  TrustCacheAddress = Address;
  AddonAddress = ALIGN_VALUE (RamdiskAddress + RamdiskBytes, SIZE_16KB);
  PolicyAddress = ALIGN_VALUE (AddonAddress + AddonSize, SIZE_16KB);
  Used = ALIGN_VALUE (PolicyAddress + PolicySize, SIZE_16KB) - Address;
  if (Used > WindowSize - BLOCK_SIZE) {
    Print (L"DARWINBOOT: used %lu window %lu kernel %lu ramdisk %lu\n", (UINT64)Used, (UINT64)WindowSize, (UINT64)(Kernel.End - Kernel.Base), (UINT64)RamdiskBytes);
    Status = EFI_BAD_BUFFER_SIZE;
    goto Fail;
  }
  ZeroMem ((VOID *)(UINTN)Address, (UINTN)Used);
  for (Index = 0; Index < Kernel.Count; Index++) {
    CopyMem ((VOID *)(UINTN)(KernelAddress + Kernel.Segments[Index].Address - Kernel.Base),
             (UINT8 *)Image + Kernel.Segments[Index].FileOffset, (UINTN)Kernel.Segments[Index].FileSize);
  }
  Args = (DARWIN_BOOT_ARGS *)(UINTN)ArgsAddress;
  Args->Revision = 2;
  Args->Version = 2;
  Args->VirtualBase = Kernel.Base - Prefix;
  Args->PhysicalBase = Address;
  Args->MemorySize = WindowSize;
  Args->MemorySizeActual = WindowSize;
  Args->TopOfKernelData = Address + Used;
  CopyMem (Args->Video, Video, sizeof (Video));
  Args->DeviceTree = Args->VirtualBase + TreeAddress - Address;
  Args->DeviceTreeLength = (UINT32)TreeSize;
  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));
  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);
  if (RootStage != NULL) {
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RootOffset), (UINTN)RootStage);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RootOffset + 8), sizeof (*RootStage));
  }
  if (Request->Ramdisk != NULL) {
    if (KeepPacked) { CopyMem ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize); }
    else { CopyRamdisk ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize, RamdiskPacked); }
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset), RamdiskAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset + 8), KeepPacked ? Request->RamdiskSize : RamdiskBytes);
  }
  if (TrustCaches != NULL) {
    CopyMem ((VOID *)(UINTN)TrustCacheAddress, TrustCaches, TrustCacheSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset), TrustCacheAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset + 8), TrustCacheMemorySize);
    FreePool (TrustCaches); TrustCaches = NULL;
  }
  if (Addon != NULL) {
    CopyMem ((VOID *)(UINTN)AddonAddress, Addon, AddonSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + AddonOffset), AddonAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + AddonOffset + 8), AddonSize);
    FreePool (Addon); Addon = NULL;
  }
  if (Policies != NULL) {
    CopyMem ((VOID *)(UINTN)PolicyAddress, Policies, PolicySize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + PolicyOffset), PolicyAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + PolicyOffset + 8), PolicySize);
    FreePool (Policies); Policies = NULL;
  }
  FreePool (Image); Image = NULL;
  FreePool (Tree); Tree = NULL;
  FreePool (TreePath); TreePath = NULL;
  Root->Close (Root); Root = NULL;
  // Every file is read; the block device is asked once more, and only for a tree that wants the record.
  if (InputsOffset != MAX_UINTN) { LocateInputs (Request, Inputs); }
  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);
  if (EFI_ERROR (Status)) { goto Fail; }
  Print (L"DARWINBOOT: ARM64 va=0x%lx pa=0x%lx args=0x%lx entry=0x%lx owned=0x%lx\n",
         Args->VirtualBase, Address, ArgsAddress, KernelAddress + Kernel.Entry - Kernel.Base, (UINT64)WindowSize);
  if (OutsideOffset != MAX_UINTN) {
    // As this map gives it; the table XNU reads comes from the map ExitBootServices accepts.
    OutsideCount = DarwinOutsideRanges (Map, MapSize, Stride, Address, WindowSize, SIZE_64KB, SIZE_1MB,
                                        (UINT8 *)(UINTN)(TreeAddress + OutsideOffset), OutsideCapacity, &OutsideBytes);
    Print (L"DARWINBOOT: free memory outside the window: %lu ranges of %lu, 0x%lx bytes\n",
           (UINT64)OutsideCount, (UINT64)OutsideCapacity, OutsideBytes);
    for (Index = 0; Index < OutsideCount; Index++) {
      Print (L"DARWINBOOT: outside 0x%lx + 0x%lx\n",
             DarwinRangeBase ((UINT8 *)(UINTN)(TreeAddress + OutsideOffset), Index),
             DarwinRangeSize ((UINT8 *)(UINTN)(TreeAddress + OutsideOffset), Index));
    }
  }
  if (InputsOffset != MAX_UINTN) {
    // The inputs as this map gives them; the records XNU reads come from the map ExitBootServices accepts.
    DarwinDescribeInputs (Map, &MapSize, Capacity, Stride, Address, WindowSize, ExcludedOffset != MAX_UINTN, Inputs);
    DarwinWriteInputs ((UINT8 *)(UINTN)(TreeAddress + InputsOffset), Inputs);
    Print (L"DARWINBOOT: payload disk 0x%lx + 0x%lx type %d state %x usable 0x%lx\n",
           Inputs[DARWIN_INPUT_PAYLOAD_DISK].Base, Inputs[DARWIN_INPUT_PAYLOAD_DISK].Size, (INT32)Inputs[DARWIN_INPUT_PAYLOAD_DISK].Type,
           Inputs[DARWIN_INPUT_PAYLOAD_DISK].State, Inputs[DARWIN_INPUT_PAYLOAD_DISK].Usable);
    Print (L"DARWINBOOT: initrd buffer 0x%lx + 0x%lx type %d state %x usable 0x%lx\n",
           Inputs[DARWIN_INPUT_INITRD].Base, Inputs[DARWIN_INPUT_INITRD].Size, (INT32)Inputs[DARWIN_INPUT_INITRD].Type,
           Inputs[DARWIN_INPUT_INITRD].State, Inputs[DARWIN_INPUT_INITRD].Usable);
  }
  if (ExcludedOffset != MAX_UINTN) {
    // The table as this map gives it; the map ExitBootServices accepts decides the one XNU reads.
    Excluded = (UINT8 *)(UINTN)(TreeAddress + ExcludedOffset);
    if (!DescribeWindow (Map, MapSize, Stride, Address, WindowSize, HeadSize, Excluded, ExcludedCapacity, Args, &ExcludedCount)) {
      Status = EFI_LOAD_ERROR;
      goto Fail;
    }
    Print (L"DARWINBOOT: window head=0x%lx usable=0x%lx excluded ranges=%lu of %lu\n",
           HeadSize, Args->MemorySizeActual, (UINT64)ExcludedCount, (UINT64)ExcludedCapacity);
    for (Index = 0; Index < ExcludedCount; Index++) {
      Print (L"DARWINBOOT: excluded 0x%lx + 0x%lx\n", DarwinRangeBase (Excluded, Index), DarwinRangeSize (Excluded, Index));
    }
  }
  Print (L"DARWINBOOT: video base=0x%lx display=%lu row=%lu width=%lu height=%lu depth=%lu\n",
         Video[0], Video[1], Video[2], Video[3], Video[4], Video[5]);
  Print (L"DARWINBOOT: command line: %a\n", CommandLine);
  Print (L"DARWINBOOT: inputs validated; exiting boot services\n");

  //
  // The same facts in large type over the whole screen. The firmware's log
  // draws far larger than the text console, and the form feed clears the
  // screen first. This is what stays visible if the kernel never draws.
  //
  DEBUG ((DEBUG_ERROR, "\fDARWINBOOT: hand-off to the kernel\n\n"));
  DEBUG ((DEBUG_ERROR, "kernel   va 0x%lx  pa 0x%lx\n", Kernel.Base, KernelAddress));
  DEBUG ((DEBUG_ERROR, "entry    pa 0x%lx\n", KernelAddress + Kernel.Entry - Kernel.Base));
  DEBUG ((DEBUG_ERROR, "args     pa 0x%lx  window 0x%lx bytes\n", ArgsAddress, (UINT64)WindowSize));
  if (Excluded != NULL) {
    DEBUG ((DEBUG_ERROR, "memory   usable 0x%lx  excluded ranges %lu\n", Args->MemorySizeActual, (UINT64)ExcludedCount));
  }
  if (InputsOffset != MAX_UINTN) {
    DEBUG ((DEBUG_ERROR, "disk     0x%lx + 0x%lx  type %d  state %x\n", Inputs[DARWIN_INPUT_PAYLOAD_DISK].Base,
            Inputs[DARWIN_INPUT_PAYLOAD_DISK].Size, (INT32)Inputs[DARWIN_INPUT_PAYLOAD_DISK].Type, Inputs[DARWIN_INPUT_PAYLOAD_DISK].State));
    DEBUG ((DEBUG_ERROR, "initrd   0x%lx + 0x%lx  type %d  state %x\n", Inputs[DARWIN_INPUT_INITRD].Base,
            Inputs[DARWIN_INPUT_INITRD].Size, (INT32)Inputs[DARWIN_INPUT_INITRD].Type, Inputs[DARWIN_INPUT_INITRD].State));
  }
  DEBUG ((DEBUG_ERROR, "video    base 0x%lx  %lux%lu  row %lu bytes  %lu bpp\n", Video[0], Video[3], Video[4], Video[2], Video[5]));
  DEBUG ((DEBUG_ERROR, "entry words %08x %08x %08x %08x\n",
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(KernelAddress + Kernel.Entry - Kernel.Base)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(KernelAddress + Kernel.Entry - Kernel.Base + 4)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(KernelAddress + Kernel.Entry - Kernel.Base + 8)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(KernelAddress + Kernel.Entry - Kernel.Base + 12))));
  DEBUG ((DEBUG_ERROR, "CurrentEL %lu  SCTLR_EL1 0x%lx  TCR_EL1 0x%lx\n", DarwinCurrentEl () >> 2, READ_SYSREG (sctlr_el1), READ_SYSREG (tcr_el1)));
  DEBUG ((DEBUG_ERROR, "ID_AA64MMFR0_EL1 0x%lx  (PARange %lu, 4 KiB granule %a)\n", READ_SYSREG (id_aa64mmfr0_el1),
          READ_SYSREG (id_aa64mmfr0_el1) & 0xF, (((READ_SYSREG (id_aa64mmfr0_el1) >> 28) & 0xF) == 0xF) ? "NOT supported" : "supported"));
  DEBUG ((DEBUG_ERROR, "ID_AA64PFR0_EL1  0x%lx\n", READ_SYSREG (id_aa64pfr0_el1)));
  DEBUG ((DEBUG_ERROR, "\nExiting boot services. A green bar across the top\nmarks the loader handoff before the kernel branch.\n"));
  if (RootStage && RootStage->WaitForHost) {
    Status=DarwinResidentRun(RootStage,ArgsAddress,KernelAddress+Kernel.Entry-Kernel.Base,
        Address,WindowSize,Video[0],Video[2]*Video[4],Map,MapSize,Stride);
    FreePool(Map); Map=NULL;
    if (EFI_ERROR(Status)) goto Fail;
    Print(L"DARWINBOOT: kernel returned; system disks retained\r\n");
    Status=((EFI_STATUS (*)(VOID))(UINTN)RootStage->WaitForHost)();
    if (EFI_ERROR(Status) && Status!=EFI_NOT_READY) goto Fail;
    goto Reload;
  }
  for (Attempt = 0; Attempt < 3; Attempt++) {
    MapSize = Capacity;
    Status = gBS->GetMemoryMap (&MapSize, Map, &Key, &Stride, &Version);
    if (EFI_ERROR (Status)) {
      if (Attempt == 0) { goto Fail; }
      CpuDeadLoop ();
    }
    // No boot service runs between this map and ExitBootServices, so the records and the tables describe the
    // memory XNU gets and the free memory outside it.
    if (OutsideOffset != MAX_UINTN) {
      OutsideCount = DarwinOutsideRanges (Map, MapSize, Stride, Address, WindowSize, SIZE_64KB, SIZE_1MB,
                                          (UINT8 *)(UINTN)(TreeAddress + OutsideOffset), OutsideCapacity, &OutsideBytes);
    }
    if (InputsOffset != MAX_UINTN) {
      DarwinDescribeInputs (Map, &MapSize, Capacity, Stride, Address, WindowSize, Excluded != NULL, Inputs);
      DarwinWriteInputs ((UINT8 *)(UINTN)(TreeAddress + InputsOffset), Inputs);
    }
    if ((Excluded != NULL) &&
        !DescribeWindow (Map, MapSize, Stride, Address, WindowSize, HeadSize, Excluded, ExcludedCapacity, Args, &ExcludedCount)) {
      if (Attempt == 0) { Status = EFI_LOAD_ERROR; goto Fail; }
      CpuDeadLoop ();
    }
    Status = gBS->ExitBootServices (mImage, Key);
    if (!EFI_ERROR (Status)) {
      if (RootOffset != MAX_UINTN) {
        DarwinEnter (ArgsAddress, KernelAddress + Kernel.Entry - Kernel.Base, Address, Used, Video[0], Video[2] * Video[4]);
        CpuDeadLoop ();
      }
      if (Excluded == NULL) {
        DarwinEnter (ArgsAddress, KernelAddress + Kernel.Entry - Kernel.Base, Address, WindowSize, Video[0], Video[2] * Video[4]);
        CpuDeadLoop ();
      }
      // The usable memory behind the head leaves the data cache as the head does in DarwinEnter. An excluded
      // range is not touched: the firmware may not even map it.
      Cursor = Address + HeadSize;
      for (Index = 0; Index < ExcludedCount; Index++) {
        if (DarwinRangeBase (Excluded, Index) > Cursor) { DarwinFlush (Cursor, DarwinRangeBase (Excluded, Index) - Cursor); }
        Cursor = DarwinRangeBase (Excluded, Index) + DarwinRangeSize (Excluded, Index);
      }
      if (Address + WindowSize > Cursor) { DarwinFlush (Cursor, Address + WindowSize - Cursor); }
      DarwinEnter (ArgsAddress, KernelAddress + Kernel.Entry - Kernel.Base, Address, HeadSize, Video[0], Video[2] * Video[4]);
      CpuDeadLoop ();
    }
    if (Status != EFI_INVALID_PARAMETER) { CpuDeadLoop (); }
  }
  CpuDeadLoop ();
  return EFI_ABORTED;
Fail:
  if (Root != NULL) { Root->Close (Root); }
  if (Image != NULL) { FreePool (Image); }
  if (Tree != NULL) { FreePool (Tree); }
  if (TrustCaches != NULL) { FreePool (TrustCaches); }
  if (Addon != NULL) { FreePool (Addon); }
  if (Policies != NULL) { FreePool (Policies); }
  if (TreePath != NULL) { FreePool (TreePath); }
  if (Map != NULL) { FreePool (Map); }
  if ((Address != 0) && (Allocated != 0)) { gBS->FreePages (Address, EFI_SIZE_TO_PAGES (Allocated)); }
  Print (L"DARWINBOOT: boot refused: %r; firmware retained\n", Status);
  return Status;
}

STATIC UNIXNU_LOADER_PROTOCOL mLoader = { UNIXNU_LOADER_REVISION, Boot };
STATIC EFI_GUID mResidentFaultGuid = ROOT_STAGE_FAULT_GUID;
extern UINT64 DarwinResidentFaultInfo[3];

STATIC
EFI_STATUS
EFIAPI
Unload (EFI_HANDLE ImageHandle)
{
  gBS->InstallConfigurationTable (&mResidentFaultGuid, NULL);
  return gBS->UninstallMultipleProtocolInterfaces (ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
}

EFI_STATUS
EFIAPI
DarwinBootEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_STATUS               Status;
  VOID                     *Existing;

  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, &Existing);
  if (!EFI_ERROR (Status)) { return EFI_ALREADY_STARTED; }
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  mImage = ImageHandle;
  Status = gBS->InstallConfigurationTable (&mResidentFaultGuid, DarwinResidentFaultInfo);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
  if (EFI_ERROR (Status)) { gBS->InstallConfigurationTable (&mResidentFaultGuid, NULL); }
  if (!EFI_ERROR (Status)) {
    Loaded->Unload = Unload;
    Print (L"DARWINBOOT: loader v2 installed EL%u\n", (UINT32)(DarwinCurrentEl () >> 2));
  }
  return Status;
}
