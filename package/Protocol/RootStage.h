#ifndef UNIXNU_ROOT_STAGE_H
#define UNIXNU_ROOT_STAGE_H

#define ROOT_STAGE_MAGIC 0x31544f4f52584e55ULL
#define ROOT_STAGE_VERSION 3
#define ROOT_STAGE_CHUNK 0x10000000ULL
#define ROOT_STAGE_EXTENTS 64
#define ROOT_STAGE_GUID {0x7db6a499,0x1639,0x46e7,{0xb2,0xca,0x28,0x5b,0x91,0xd0,0x6f,0x21}}
#define ROOT_STAGE_FAULT_GUID {0x1d7d3ce9,0x52a6,0x49c1,{0xb8,0x6e,0x35,0xb6,0x82,0x72,0x98,0xa4}}
#define ROOT_STAGE_SERVICE_GUID {0x773d7f84,0x6185,0x4b98,{0xa3,0x16,0xec,0xb1,0xd8,0x2f,0x50,0x69}}
#define ROOT_STAGE_SERVICE_MAGIC 0x3156534255535452ULL
#define ROOT_STAGE_USB_PHY_CALIBRATION_MAGIC 0x31594850u
#define ROOT_STAGE_USB_PHY_CALIBRATION_OFFSETS \
  {0x54,0xb8,0x5c,0x60,0x58,0x68,0x6c,0x70,0x7c,0x78,0x64,0x3c}

// Resident transport hooks run after loader allocations and before firmware
// services resume. The transport retains its controller and descriptors.
typedef struct {
  // Each cache line has one writer. Firmware publishes requests and heartbeat;
  // XNU publishes completion and readiness in the other line.
  volatile unsigned int Sequence;
  volatile unsigned int Command;
  volatile unsigned int Heartbeat;
  unsigned char Reserved[52];
} __attribute__((aligned(64))) ROOT_STAGE_USB_HOST;

typedef struct {
  volatile unsigned int Completed;
  volatile unsigned int Ready;
  unsigned char Reserved[56];
} __attribute__((aligned(64))) ROOT_STAGE_USB_KERNEL;

typedef struct {
  unsigned long long Magic;
  unsigned long long Enter;
  unsigned long long Resume;
} ROOT_STAGE_SERVICE;

typedef struct {
  unsigned long long Address;
  unsigned long long Bytes;
} ROOT_STAGE_EXTENT;

typedef struct {
  unsigned long long Bytes;
  unsigned long long Received;
  unsigned long long Count;
  char Identity[33];
  ROOT_STAGE_EXTENT Extents[ROOT_STAGE_EXTENTS];
} ROOT_STAGE_DISK;

typedef struct {
  unsigned long long Magic;
  unsigned long long Version;
  unsigned long long KernelBase;
  unsigned long long KernelBytes;
  unsigned long long DiskCount;
  unsigned long long ReturnStub;
  unsigned long long WaitForHost;
  unsigned long long ReturnTest;
  unsigned long long ReturnCount;
  ROOT_STAGE_DISK Disks[3];
  ROOT_STAGE_USB_HOST UsbHost;
  ROOT_STAGE_USB_KERNEL UsbKernel;
} ROOT_STAGE_TABLE;

#endif
