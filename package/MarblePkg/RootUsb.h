#ifndef MARBLE_ROOT_USB_H
#define MARBLE_ROOT_USB_H

// Qualcomm EFI_USB_DEVICE_PROTOCOL wire ABI, shared with the working marble loader.
enum { UsbDeviceEventNoEvent, UsbDeviceEventDeviceStateChange, UsbDeviceEventTransferNotification, UsbDeviceEventOemEvent };
enum { UsbDeviceTransferStatusActive, UsbDeviceTransferStatusCompleteOK, UsbDeviceTransferStatusCancelled, UsbDeviceTransferStatusCompleteError };
typedef UINT32 USB_DEVICE_EVENT;
typedef union {
  UINT32 DeviceState;
  struct { UINT32 Status; UINT8 EndpointIndex; UINTN BytesCompleted; VOID *DataBuffer; } TransferOutcome;
  UINT64 Raw[8];
} USB_DEVICE_EVENT_DATA;
typedef struct {
  VOID *DeviceDescriptor;
  VOID **Descriptors;
  VOID *SSDeviceDescriptor;
  VOID **SSDescriptors;
  VOID *DeviceQualifierDescriptor;
  VOID *BinaryDeviceObjectStore;
  UINT8 StringDescriptorCount;
  VOID **StringDescriptors;
} ROOT_USB_DESCRIPTOR_SET;
typedef struct {
  UINTN Revision;
  VOID *Start;
  EFI_STATUS (*Send)(UINT8,UINTN,VOID *);
  EFI_STATUS (EFIAPI *HandleEvent)(USB_DEVICE_EVENT *,UINTN *,USB_DEVICE_EVENT_DATA *);
  EFI_STATUS (EFIAPI *AllocateTransferBuffer)(UINTN,VOID **);
  EFI_STATUS (EFIAPI *FreeTransferBuffer)(VOID *);
  EFI_STATUS (*Stop)(VOID);
  EFI_STATUS (*AbortXfer)(UINT8);
  EFI_STATUS (EFIAPI *SetEndpointStallState)(UINT8,BOOLEAN);
  EFI_STATUS (*StartEx)(ROOT_USB_DESCRIPTOR_SET *);
} ROOT_USB_PROTOCOL;

#endif
