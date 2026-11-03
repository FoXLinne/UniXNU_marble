// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/RootStage.h>

STATIC ROOT_STAGE_TABLE *mStorage;
EFI_STATUS MarbleRootUsbResume(ROOT_STAGE_TABLE *Stage);
EFI_STATUS MarbleRootUsbPrepare(ROOT_STAGE_TABLE *Stage);
STATIC EFI_STATUS StorageReturn(VOID) { return MarbleRootUsbResume(mStorage); }

EFI_STATUS MarbleRootStorage(VOID) {
  EFI_HANDLE *Handles=NULL; UINTN Count=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  EFI_BLOCK_IO_PROTOCOL *Block=NULL;
  UINT64 First=0, Last=0;
  UINT8 *Gpt=AllocateAlignedPages(4,4096);
  if (!Gpt) { FreePool(Handles); return EFI_OUT_OF_RESOURCES; }
  for (UINTN I=0;I<Count;I++) {
    EFI_BLOCK_IO_PROTOCOL *Candidate=NULL;
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Candidate)) ||
        !Candidate->Media->MediaPresent || Candidate->Media->LogicalPartition ||
        Candidate->Media->BlockSize!=4096 || Candidate->Media->LastBlock<0x800000) continue;
    if (EFI_ERROR(Candidate->ReadBlocks(Candidate,Candidate->Media->MediaId,1,4096,Gpt)) ||
        CompareMem(Gpt,"EFI PART",8)) continue;
    UINT64 EntryLba=ReadUnaligned64((UINT64 *)(Gpt+72));
    UINT32 Entries=ReadUnaligned32((UINT32 *)(Gpt+80)), EntryBytes=ReadUnaligned32((UINT32 *)(Gpt+84));
    if (!Entries || Entries>128 || EntryBytes!=128 || EntryLba<2 || EntryLba>64) continue;
    UINTN Bytes=ALIGN_VALUE(Entries*EntryBytes,4096);
    if (EFI_ERROR(Candidate->ReadBlocks(Candidate,Candidate->Media->MediaId,EntryLba,Bytes,Gpt))) continue;
    for (UINTN J=0;J<Entries;J++) {
      UINT8 *Entry=Gpt+J*128;
      if (CompareMem(Entry+56,L"unixnu",sizeof(L"unixnu"))) continue;
      First=ReadUnaligned64((UINT64 *)(Entry+32)); Last=ReadUnaligned64((UINT64 *)(Entry+40));
      if (First<6 || Last<First || Last>=Candidate->Media->LastBlock) continue;
      Block=Candidate; break;
    }
    if (Block) break;
  }
  FreePages(Gpt,4); FreePool(Handles);
  if (!Block) return EFI_NOT_FOUND;
  UINT32 Sector=Block->Media->BlockSize;
  UINT8 *Header=AllocateAlignedPages(1,MAX((UINTN)4096,(UINTN)Block->Media->IoAlign));
  if (!Header) return EFI_OUT_OF_RESOURCES;
  Status=Block->ReadBlocks(Block,Block->Media->MediaId,First,4096,Header);
  if (EFI_ERROR(Status)) { FreePages(Header,1); return Status; }
  if (CompareMem(Header,"SHNGROOT",8) || ReadUnaligned32((UINT32 *)(Header+8))!=1 ||
      ReadUnaligned32((UINT32 *)(Header+12))!=3) { FreePages(Header,1); return EFI_COMPROMISED_DATA; }
  UINT64 ImageBytes=ReadUnaligned64((UINT64 *)(Header+16));
  if (ImageBytes/Sector>Last-First+1 || ImageBytes%4096) { FreePages(Header,1); return EFI_BAD_BUFFER_SIZE; }
  mStorage=AllocatePages(EFI_SIZE_TO_PAGES(sizeof(*mStorage)));
  if (!mStorage) { FreePages(Header,1); return EFI_OUT_OF_RESOURCES; }
  ZeroMem(mStorage,sizeof(*mStorage));
  mStorage->Magic=ROOT_STAGE_MAGIC; mStorage->Version=2; mStorage->DiskCount=2;
  mStorage->KernelBytes=0x100000000ULL;
  mStorage->KernelBase=(UINTN)AllocateAlignedPages(EFI_SIZE_TO_PAGES(mStorage->KernelBytes+0x200000),0x200000);
  if (!mStorage->KernelBase) { FreePages(Header,1); return EFI_OUT_OF_RESOURCES; }
  mStorage->WaitForHost=(UINTN)&StorageReturn;
  CONST CHAR8 *Names[]={"OS","SystemOS","KernelSeed"};
  for (UINTN I=0;I<3;I++) {
    UINT8 *Entry=Header+24+I*128;
    UINT64 Offset=ReadUnaligned64((UINT64 *)(Entry+32)), Bytes=ReadUnaligned64((UINT64 *)(Entry+40));
    if (CompareMem(Entry,Names[I],AsciiStrLen(Names[I])+1) || !Bytes || Bytes%512 || Offset%4096 ||
        Offset<4096 || Offset>ImageBytes || Bytes>ImageBytes-Offset) { Status=EFI_COMPROMISED_DATA; goto Done; }
    ROOT_STAGE_DISK *Disk=&mStorage->Disks[I]; Disk->Bytes=Bytes; Disk->Received=Bytes;
    for (UINTN J=0;J<16;J++) {
      Disk->Identity[J*2]="0123456789abcdef"[Entry[56+J]>>4];
      Disk->Identity[J*2+1]="0123456789abcdef"[Entry[56+J]&15];
    }
    if (I<2) continue;
    if (Bytes>0xc000000 || Bytes%4096) { Status=EFI_BAD_BUFFER_SIZE; goto Done; }
    UINT8 *Data=AllocateAlignedPages(EFI_SIZE_TO_PAGES(Bytes),MAX((UINTN)4096,(UINTN)Block->Media->IoAlign));
    if (!Data) { Status=EFI_OUT_OF_RESOURCES; goto Done; }
    Disk->Count=1; Disk->Extents[0]=(ROOT_STAGE_EXTENT){(UINTN)Data,Bytes};
    for (UINT64 At=0;At<Bytes;At+=0x100000) {
      UINTN Size=(UINTN)MIN((UINT64)0x100000,Bytes-At);
      Status=Block->ReadBlocks(Block,Block->Media->MediaId,First+(Offset+At)/Sector,Size,Data+At);
      if (EFI_ERROR(Status)) goto Done;
    }
  }
  {
    EFI_GUID Guid=ROOT_STAGE_GUID;
    Status=gBS->InstallConfigurationTable(&Guid,mStorage);
    if (!EFI_ERROR(Status)) {
      EFI_STATUS UsbStatus=MarbleRootUsbPrepare(mStorage);
      if (EFI_ERROR(UsbStatus)) Print(L"RootStage USB: %r\r\n",UsbStatus);
    }
    Print(L"UniXNU UFS boot\r\n");
  }
Done:
  FreePages(Header,1); return Status;
}
