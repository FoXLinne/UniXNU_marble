#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <sys/param.h>
#include <sys/msgbuf.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IOBSD.h>
#include <Protocol/RootStage.h>
#include <arm/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <libkern/crypto/sha2.h>
#include <libkern/zlib.h>
#include "SparseDisk.h"

extern "C" {
#include <vm/pmap.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/task.h>
#include <arm/thread.h>
#include <sys/systm.h>

extern kmod_info_t g_kernel_kmod_info;
void native_runtime_usb_start(void);
void native_runtime_usb_report(char *, unsigned);
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void root_block_static_init(void) __asm__("__GLOBAL__sub_I_NativeRootBlock.cpp");
volatile uint64_t native_root_block_reads;
char native_root_block_panic_text[4096];
char *native_root_block_boot_args;
}

static constexpr size_t rootLogBytes = 65536;
static constexpr size_t rootOutputBytes = 1048576;
static IOMemoryMap *rootLogMapping;
static void (*rootObserver)(unsigned);
static uint32_t observationSeconds;
extern "C" void native_root_block_register_observer(void (*observer)(unsigned))
{
  rootObserver = observer;
}

extern "C" void native_root_block_trace_write(const char *text)
{
  if (!rootLogMapping) return;
  constexpr size_t offset = 800 * 1024;
  constexpr size_t capacity = rootOutputBytes - offset - 16;
  static uint32_t used;
  uint8_t *output = (uint8_t *)rootLogMapping->getVirtualAddress() + offset;
  size_t length = strlen(text);
  if (length + 1 > capacity - used) return;
  memcpy(output + 16 + used, text, length);
  used += length;
  output[16 + used++] = '\n';
  uint32_t version = 1;
  memcpy(output + 8, &version, 4);
  memcpy(output + 12, &used, 4);
  memcpy(output, "ROOTTRC1", 8);
}

static void rootObservationWorker(void *, wait_result_t)
{
  rootObserver(observationSeconds);
  thread_terminate(current_thread());
}
static const volatile uint8_t *displayPixels;
static uint64_t displayStride, displayFrames;
static uint32_t displayWidth, displayHeight;
static uint64_t firstDisplayTime;
extern "C" void native_root_block_register_display(uint64_t address, uint64_t stride, uint32_t width, uint32_t height)
{
  displayPixels = (const volatile uint8_t *)address;
  displayStride = stride;
  displayWidth = width;
  displayHeight = height;
}

extern "C" void native_root_block_presented(void)
{
  __atomic_fetch_add(&displayFrames, 1, __ATOMIC_RELAXED);
  if (!firstDisplayTime) firstDisplayTime = mach_absolute_time();
}

static void *sourceSnapshot;
static size_t sourceSnapshotBytes;
static void capturePixels(void *output, const volatile uint8_t *pixels,
    uint64_t stride, uint32_t width, uint32_t height, uint64_t frames)
{
  struct ScreenHeader {
    char magic[8];
    uint32_t width, height, stride, bytes;
    uint64_t frames;
  } header;
  memcpy(header.magic, "ROOTRGB1", 8);
  uint32_t step = MAX((width + 319) / 320, (height + 719) / 720);
  header.width = (width + step - 1) / step;
  header.height = (height + step - 1) / step;
  header.stride = header.width * 3;
  header.bytes = header.stride * header.height;
  header.frames = frames;
  auto *target = (uint8_t *)output;
  for (uint32_t y = 0; y < header.height; y++) {
    const volatile uint8_t *source = pixels + y * step * stride;
    uint8_t *row = target + sizeof(header) + y * header.stride;
    for (uint32_t x = 0; x < header.width; x++) {
      const volatile uint8_t *pixel = source + x * step * 4;
      row[x * 3] = pixel[2];
      row[x * 3 + 1] = pixel[1];
      row[x * 3 + 2] = pixel[0];
    }
  }
  memcpy(output, &header, sizeof(header));
}
static void captureRootScreen(void)
{
  if (!rootLogMapping || !displayPixels) return;
  capturePixels((uint8_t *)rootLogMapping->getVirtualAddress()+rootLogBytes,
      displayPixels,displayStride,displayWidth,displayHeight,displayFrames);
}
extern "C" void native_root_block_capture_surface(const void *pixels, uint64_t stride, uint32_t width, uint32_t height)
{
  if (!sourceSnapshot) sourceSnapshot = IOMalloc(32 + 320 * 720 * 3);
  if (!sourceSnapshot) return;
  capturePixels(sourceSnapshot,(const volatile uint8_t *)pixels,stride,width,height,displayFrames+1);
  uint32_t length;
  memcpy(&length,(uint8_t *)sourceSnapshot+20,4);
  sourceSnapshotBytes=32+length;
}

static RamSparseDisk dataSeed = {};
static IOBufferMemoryDescriptor *dataExportIndex;
extern "C" bool native_root_block_data_is_ephemeral(void) { return dataSeed.pages != nullptr; }
static uint64_t dataSeedBytes;
static uint8_t dataSeedHash[32];
static char dataSeedUUID[37];
static char panicProcess[32];
static char residentBootArgs[BOOT_LINE_LENGTH];
static struct { uint64_t bytes; char identity[33]; } rootIdentities[2];
static thread_call_t observationCall;

extern "C" void native_root_block_export_data(void)
{
  if (!rootLogMapping || !dataExportIndex) return;
  struct DataHeader {
    char magic[8];
    uint32_t version, pageSize;
    uint64_t logical, indexAddress, indexBytes;
    char uuid[37];
  } header = {};
  auto *index = (uint64_t *)dataExportIndex->getBytesNoCopy();
  for (uint64_t i=0; i<dataSeed.bytes/4096; i++) {
    uint8_t *page=dataSeed.pages[i];
    index[i]=page ? ml_vtophys((vm_offset_t)page) : 0;
    if (page) flush_dcache64((addr64_t)page,4096,FALSE);
  }
  memcpy(header.magic,"ROOTDAT1",8);
  header.version=1; header.pageSize=4096; header.logical=dataSeed.bytes;
  header.indexAddress=dataExportIndex->getPhysicalSegment(0,nullptr);
  header.indexBytes=dataSeed.bytes/4096*8;
  memcpy(header.uuid,dataSeedUUID,sizeof(header.uuid));
  flush_dcache64((addr64_t)index,header.indexBytes,FALSE);
  auto *out=(uint8_t *)rootLogMapping->getVirtualAddress()+768*1024;
  memcpy(out,&header,sizeof(header));
  flush_dcache64((addr64_t)out,sizeof(header),FALSE);
}

static voidpf dataInflateAllocate(voidpf, uInt items, uInt size)
{
  size_t bytes = (size_t)items * size + 16;
  auto *block = (uint8_t *)IOMallocZero(bytes);
  if (!block) return Z_NULL;
  memcpy(block, &bytes, sizeof(bytes));
  return block + 16;
}

static void dataInflateFree(voidpf, voidpf address)
{
  auto *block = (uint8_t *)address - 16;
  size_t bytes;
  memcpy(&bytes, block, sizeof(bytes));
  IOFree(block, bytes);
}

extern "C" void native_root_block_process_exit(const char *name, int signal, int exception,
    uint64_t code, uint64_t subcode, uint32_t reasonNamespace, uint64_t reasonCode, const char *reason)
{
  bool selected = false;
  for (const char *start = panicProcess; *start;) {
    const char *end = strchr(start, ',');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (strlen(name) == length && !strncmp(start, name, length)) selected = true;
    start += length + (end != nullptr);
  }
  if (!selected) return;
  arm_saved_state_t *state = find_user_regs(current_thread());
  uint64_t pc = state ? get_saved_state_pc(state) : 0;
  uint64_t lr = state ? get_saved_state_lr(state) : 0;
  uint64_t fp = state ? get_saved_state_fp(state) : 0;
  char trace[2048];
  size_t used = snprintf(trace, sizeof(trace), "pc=%llx lr=%llx fp=%llx sp=%llx\n",
      pc, lr, fp, state ? get_saved_state_sp(state) : 0);
  if (state) {
    for (unsigned i = 0; i < 8; i++)
      used += snprintf(trace + used, sizeof(trace) - used, "x%u=%llx ", i, get_saved_state_reg(state, i));
  }
  uint64_t infos[24];
  user_addr_t infoAddress = task_get_all_image_info_addr(current_task());
  if (infoAddress && !copyin(infoAddress, infos, sizeof(infos)))
    used += snprintf(trace + used, sizeof(trace) - used, "\ncache_slide=%llx cache_base=%llx\n", infos[19], infos[22]);
  for (unsigned i = 0; i < 12 && fp && !(fp & 15); i++) {
    uint64_t frame[2];
    if (copyin(fp, frame, sizeof(frame))) break;
    used += snprintf(trace + used, sizeof(trace) - used, "frame %u fp=%llx return=%llx\n", i, fp, frame[1]);
    if (frame[0] <= fp) break;
    fp = frame[0];
  }
  captureRootScreen();
  panic("RootStage: %s exit signal=%d exception=%d code=%llx subcode=%llx namespace=%u reason=%llx %s\n%s",
      name, signal, exception, code, subcode, reasonNamespace, reasonCode, reason ? reason : "", trace);
}

static void observationExpired(thread_call_param_t, thread_call_param_t)
{
  captureRootScreen();
  panic("RootStage observation timeout after %u seconds Data=%llu/%llu copies=%llu", observationSeconds, dataSeed.resident, dataSeed.bytes, displayFrames);
}

extern "C" void native_root_block_capture_log(void)
{
  if (!rootLogMapping) return;
  static struct {
    char magic[8];
    uint32_t version, length;
    char text[rootLogBytes - 16];
  } log;
  memcpy(log.magic, "ROOTLOG1", sizeof(log.magic));
  log.version = 1;
  size_t used = 0;
  size_t length = MIN((size_t)msgbufp->msg_size, sizeof(log.text) - sizeof(native_root_block_panic_text) - 512);
  size_t start = (msgbufp->msg_bufx + msgbufp->msg_size - length) % msgbufp->msg_size;
  for (size_t i = 0; i < length; i++) {
    char c = msgbufp->msg_bufc[(start + i) % msgbufp->msg_size];
    if (c) log.text[used++] = c;
  }
  if (native_root_block_panic_text[0])
    used += snprintf(log.text + used, sizeof(log.text) - used, "\nPANIC\n%s\n", native_root_block_panic_text);
  char runtime[256];
  native_runtime_usb_report(runtime,sizeof(runtime));
  used += snprintf(log.text + used, sizeof(log.text) - used, "%s\n",runtime);
  log.length = (uint32_t)used;
  void *output = (void *)rootLogMapping->getVirtualAddress();
  memcpy(output, &log, sizeof(log));
  flush_dcache64((addr64_t)output, rootOutputBytes, FALSE);
}

extern "C" unsigned native_root_runtime_read(unsigned kind, unsigned offset, void *out, unsigned capacity)
{
  if (!rootLogMapping) return 0;
  if (kind == 5) {
    uint64_t now, first=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&now);
    if (firstDisplayTime) absolutetime_to_nanoseconds(firstDisplayTime,&first);
    struct {
      char magic[8];
      uint64_t uptime, firstFrame, reads, frames, resident, logical;
    } status;
    memcpy(status.magic,"ROOTRUN1",8);
    status.uptime=now; status.firstFrame=first; status.reads=native_root_block_reads;
    status.frames=displayFrames; status.resident=dataSeed.resident; status.logical=dataSeed.bytes;
    unsigned count=MIN(capacity,(unsigned)sizeof(status));
    memcpy(out,&status,count);
    return count;
  }
  if (kind == 6) {
    if (offset >= sourceSnapshotBytes) return 0;
    unsigned count = MIN((size_t)capacity, sourceSnapshotBytes-offset);
    memcpy(out,(uint8_t *)sourceSnapshot+offset,count);
    return count;
  }
  if (!offset) {
    if (kind == 1) native_root_block_capture_log();
    else captureRootScreen();
  }
  size_t base = kind == 1 ? 0 : rootLogBytes;
  size_t length = rootLogBytes;
  if (kind == 2) {
    uint32_t pixels;
    memcpy(&pixels, (uint8_t *)rootLogMapping->getVirtualAddress() + base + 20, 4);
    length = 32 + pixels;
    if (length > 800 * 1024 - rootLogBytes) return 0;
  }
  if (offset >= length) return 0;
  unsigned count = MIN((size_t)capacity, length - offset);
  memcpy(out, (uint8_t *)rootLogMapping->getVirtualAddress() + base + offset, count);
  return count;
}

class NativeRootBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeRootBlock)
  ROOT_STAGE_DISK disk = {};
  bool online = false;
  unsigned diskIndex = 0;
  RamSparseDisk *writableData = nullptr;
  IOLock *dataGate = nullptr;
public:
  void configure(const ROOT_STAGE_DISK &source, unsigned index) {
    if (!source.Bytes || source.Received != source.Bytes ||
        source.Count != (source.Bytes + ROOT_STAGE_CHUNK - 1) / ROOT_STAGE_CHUNK ||
        source.Count > ROOT_STAGE_EXTENTS)
      panic("RootStage: disk %u metadata bytes=%llu received=%llu extents=%llu",
          index, source.Bytes, source.Received, source.Count);
    disk = source;
    diskIndex = index;
    uint64_t total = 0;
    for (unsigned i = 0; i < disk.Count; i++) {
      uint64_t expected = MIN(ROOT_STAGE_CHUNK, disk.Bytes - total);
      if (!disk.Extents[i].Address || disk.Extents[i].Bytes != expected)
        panic("RootStage: disk %u extent %u invalid pa=%llx bytes=%llu expected=%llu",
            index, i, disk.Extents[i].Address, disk.Extents[i].Bytes, expected);
      total += expected;
    }
  }
  void configureData(RamSparseDisk *data) {
    writableData = data;
    disk.Bytes = data->bytes;
    diskIndex = 2;
    dataGate = IOLockAlloc();
    if (!dataGate) panic("RootStage: Data I/O lock allocation failed");
  }
  int sourceIndex() const { return online ? (int)diskIndex : -1; }
  bool start(IOService *provider) override {
    if (!IOBlockStorageDevice::start(provider)) return false;
    online = true;
    return true;
  }
  void stop(IOService *provider) override {
    online = false;
    IOBlockStorageDevice::stop(provider);
  }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("UniXNU"); }
  char *getProductString() override { return const_cast<char *>("RootStage"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(""); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = disk.Bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = !writableData; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
      IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!online) return kIOReturnNotReady;
    bool write = buffer->getDirection() == kIODirectionOut;
    if (write && !writableData) return kIOReturnNotWritable;
    uint64_t capacity = disk.Bytes / 512;
    if (block > capacity || count > capacity - block || count * 512 > buffer->getLength()) return kIOReturnBadArgument;
    uint64_t offset = block * 512, bytes = count * 512, done = 0;
    if (writableData) {
      struct DataOps {
        IOMemoryDescriptor *buffer;
        uint8_t *allocate() { return (uint8_t *)IOMallocZero(4096); }
        uint64_t read(uint64_t at, void *p, uint64_t n) { return buffer->readBytes(at, p, n); }
        uint64_t copy(uint64_t at, const void *p, uint64_t n) { return buffer->writeBytes(at, p, n); }
      } ops = {buffer};
      bool noMemory;
      IOLockLock(dataGate);
      done = ramSparseIO(*writableData, write, offset, bytes, ops, noMemory);
      IOLockUnlock(dataGate);
      IOReturn result = done == bytes ? kIOReturnSuccess : noMemory ? kIOReturnNoMemory : kIOReturnIOError;
      completion->action(completion->target, completion->parameter, result, done);
      return kIOReturnSuccess;
    }
    while (done < bytes) {
      uint64_t position = offset + done;
      unsigned index = position / ROOT_STAGE_CHUNK;
      uint64_t within = position % ROOT_STAGE_CHUNK;
      uint64_t size = MIN(bytes - done, disk.Extents[index].Bytes - within);
      IOByteCount destinationBytes;
      addr64_t destination = buffer->getPhysicalSegment(done, &destinationBytes, kIOMemoryMapperNone);
      if (!destination || !destinationBytes) break;
      size = MIN(size, destinationBytes);
      // XNU's copy windows cover staged RAM outside the managed kernel window.
      if (copypv(disk.Extents[index].Address + within, destination,
          (unsigned int)size, cppvPsrc | cppvPsnk | cppvNoRefSrc) != KERN_SUCCESS) break;
      done += size;
    }
    __atomic_fetch_add(&native_root_block_reads, 1, __ATOMIC_RELAXED);
    completion->action(completion->target, completion->parameter,
        done == bytes ? kIOReturnSuccess : kIOReturnIOError, done);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(NativeRootBlock, IOBlockStorageDevice)

extern "C" void native_root_block_start(void)
{
  static bool started;
  if (started) return;
  IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  OSData *range = map ? OSDynamicCast(OSData, map->getProperty("RootDisks")) : nullptr;
  if (!range) { OSSafeReleaseNULL(map); return; }
  uint64_t location[2];
  if (range->getLength() != sizeof(location)) panic("RootStage: invalid memory-map property");
  memcpy(location, range->getBytesNoCopy(), sizeof(location));
  map->release();
  if (location[1] != sizeof(ROOT_STAGE_TABLE)) panic("RootStage: invalid table size");
  // These RAM ranges are CPU-backed storage, independent of a DMA mapper.
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
      location[0],location[1],kIODirectionIn | kIOMemoryMapperNone,nullptr);
  IOMemoryMap *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapReadOnly) : nullptr;
  OSSafeReleaseNULL(memory);
  if (!mapping) panic("RootStage: table mapping failed");
  const auto *table = reinterpret_cast<const ROOT_STAGE_TABLE *>(mapping->getVirtualAddress());
  if (table->Magic != ROOT_STAGE_MAGIC || table->Version != 2) panic("RootStage: invalid table header");
  extern void native_resident_configure(uint64_t);
  native_resident_configure(table->ReturnStub);
  const ROOT_STAGE_DISK &kernel = table->Disks[2];
  if (kernel.Count == 1 && kernel.Bytes >= rootOutputBytes && kernel.Bytes <= kernel.Extents[0].Bytes) {
    uint64_t address = kernel.Extents[0].Address + kernel.Bytes - rootOutputBytes;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
        address, rootOutputBytes, kIODirectionInOut | kIOMemoryMapperNone, nullptr);
    IOMemoryMap *log = memory ? memory->map(kIOMapAnywhere | kIOMapCopybackCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!log) panic("RootStage: log mailbox mapping failed");
    if (!memcmp((const void *)log->getVirtualAddress(), "ROOTLOG1", 8)) {
      rootLogMapping = log;
      const char *mailbox = (const char *)log->getVirtualAddress();
      memcpy(&observationSeconds, mailbox + 32, sizeof(observationSeconds));
      uint64_t offset, bytes, logical;
      memcpy(&offset, mailbox + 40, sizeof(offset));
      memcpy(&bytes, mailbox + 48, sizeof(bytes));
      memcpy(&logical, mailbox + 56, sizeof(logical));
      const char *extraArgs = mailbox + 128;
      size_t extraLength = strnlen(extraArgs, BOOT_LINE_LENGTH);
      if (extraLength) {
        strlcpy(residentBootArgs, PE_boot_args(), sizeof(residentBootArgs));
        char *commandLine = residentBootArgs;
        if (extraLength >= BOOT_LINE_LENGTH || strlen(commandLine) + extraLength + 2 > BOOT_LINE_LENGTH)
          panic("RootStage: extra boot arguments exceed the command line limit");
        strlcat(commandLine, " ", BOOT_LINE_LENGTH);
        strlcat(commandLine, extraArgs, BOOT_LINE_LENGTH);
        native_root_block_boot_args = residentBootArgs;
        IOLog("RootStage: extra boot arguments %s\n", extraArgs);
      }
      PE_parse_boot_argn("rootstage-panic-process", panicProcess, sizeof(panicProcess));
      if (bytes) {
        memcpy(dataSeedUUID, mailbox + 64, sizeof(dataSeedUUID));
        if (strnlen(dataSeedUUID, sizeof(dataSeedUUID)) != 36) panic("RootStage: Data volume UUID invalid");
        if ((offset & 4095) || !logical || logical > 0x10000000ULL || offset > kernel.Bytes - rootOutputBytes ||
            bytes > kernel.Bytes - rootOutputBytes - offset)
          panic("RootStage: Data seed range invalid");
        void *data = IOMalloc(logical);
        size_t copyBytes = (bytes + 4095) & ~4095ULL;
        void *packed = IOMalloc(copyBytes);
        if (!data || !packed) panic("RootStage: Data allocation failed for %llu bytes", logical);
        if (copypv(kernel.Extents[0].Address + offset, (addr64_t)packed, (unsigned int)copyBytes,
            cppvPsrc | cppvNoRefSrc) != KERN_SUCCESS)
          panic("RootStage: Data seed copy failed");
        z_stream stream = {};
        stream.zalloc = dataInflateAllocate;
        stream.zfree = dataInflateFree;
        stream.next_in = (Bytef *)packed;
        stream.avail_in = (uInt)bytes;
        stream.next_out = (Bytef *)data;
        stream.avail_out = (uInt)logical;
        if (inflateInit(&stream) != Z_OK) panic("RootStage: Data inflater initialization failed");
        int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        IOFree(packed, copyBytes);
        if (result != Z_STREAM_END || stream.total_out != logical || stream.avail_in)
          panic("RootStage: Data seed decompression failed: %d", result);
        if (!ramPackedLayout((const uint8_t *)data, logical, &dataSeedBytes) || dataSeedBytes > 0x80000000ULL)
          panic("RootStage: packed Data layout invalid");
        dataSeed.bytes = dataSeedBytes;
        dataSeed.pages = (uint8_t **)IOMallocZero((dataSeedBytes / 4096) * sizeof(uint8_t *));
        struct ImportOps { uint8_t *allocate() { return (uint8_t *)IOMallocZero(4096); } } ops;
        if (!ramSparseImport(dataSeed, (const uint8_t *)data, logical, ops))
          panic("RootStage: sparse Data import failed");
        IOFree(data, logical);
        static const uint8_t zero[4096] = {};
        SHA256_CTX context;
        SHA256_Init(&context);
        for (uint64_t i = 0; i < dataSeedBytes / 4096; i++)
          SHA256_Update(&context, dataSeed.pages[i] ? dataSeed.pages[i] : zero, 4096);
        SHA256_Final(dataSeedHash, &context);
      }
    } else log->release();
  }
  kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  if (result) panic("RootStage: IOStorageFamily load returned %x",result);
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  root_block_static_init();
  result = OSMetaClass::postModLoad(handle);
  if (result) panic("RootStage: class registration returned %x",result);
  result = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
  if (result) panic("RootStage: APFS load returned %x",result);
  IOService *parent = IOService::getPlatform();
  if (!parent) panic("RootStage: platform is unavailable");
  for (unsigned i = 0; i < table->DiskCount; i++) {
    auto *disk = new NativeRootBlock;
    if (!disk) panic("RootStage: disk %u allocation failed", i);
    if (!disk->init(nullptr)) panic("RootStage: disk %u initialization failed", i);
    disk->configure(table->Disks[i], i);
    rootIdentities[i].bytes = table->Disks[i].Bytes;
    memcpy(rootIdentities[i].identity, table->Disks[i].Identity, sizeof(rootIdentities[i].identity));
    if (!disk->attach(parent)) panic("RootStage: disk %u attachment failed", i);
    if (!disk->start(parent)) panic("RootStage: disk %u service start failed", i);
    disk->setName(i ? "SystemOS" : "OS");
    disk->registerService(kIOServiceSynchronous);
    IOLog("RootStage: disk %u published, %llu bytes\n",i,table->Disks[i].Bytes);
  }
  if (dataSeed.pages) {
    unsigned exportData=0;
    if (PE_parse_boot_argn("rootstage-export-data",&exportData,sizeof(exportData)) && exportData==1) {
      dataExportIndex=IOBufferMemoryDescriptor::withOptions(kIODirectionInOut|kIOMemoryPhysicallyContiguous,
          dataSeed.bytes/4096*8,4096);
      if (!dataExportIndex) panic("RootStage: Data export index allocation failed");
    }
    auto *disk = new NativeRootBlock;
    if (!disk || !disk->init(nullptr)) panic("RootStage: Data service allocation failed");
    disk->configureData(&dataSeed);
    if (!disk->attach(parent) || !disk->start(parent)) panic("RootStage: Data service start failed");
    disk->setName("Data");
    disk->registerService(kIOServiceSynchronous);
    IOLog("RootStage: writable Data published, %llu logical bytes, %llu resident bytes\n", dataSeedBytes, dataSeed.resident);
  }
  mapping->release();
  started = true;
  native_runtime_usb_start();
}

extern "C" const char *native_root_block_report(void)
{
  static char report[2048];
  size_t used = snprintf(report, sizeof(report), "reads=%llu\n", native_root_block_reads);
  // Preserve the final kernel messages across the return to the USB loader.
  const size_t length = MIN((size_t)msgbufp->msg_size, sizeof(report) - used - 1);
  size_t start = (msgbufp->msg_bufx + msgbufp->msg_size - length) % msgbufp->msg_size;
  for (size_t i = 0; i < length; i++) {
    char c = msgbufp->msg_bufc[(start + i) % msgbufp->msg_size];
    if (c) report[used++] = c;
  }
  report[used] = '\0';
  return report;
}

static int rootBlockIndex(dev_t device)
{
  IORegistryIterator *iterator = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
  if (!iterator) return -1;
  int index = -1;
  while (IORegistryEntry *entry = iterator->getNextObject()) {
    OSNumber *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
    OSNumber *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
    if (!majorNumber || !minorNumber || majorNumber->unsigned32BitValue() != (unsigned)major(device) ||
        minorNumber->unsigned32BitValue() != (unsigned)minor(device)) continue;
    for (IORegistryEntry *parent = entry; parent; parent = parent->getParentEntry(gIOServicePlane)) {
      NativeRootBlock *disk = OSDynamicCast(NativeRootBlock, parent);
      if (disk) { index = disk->sourceIndex(); break; }
    }
    break;
  }
  iterator->release();
  return index;
}

extern "C" int native_root_block_identity_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
  int index = rootBlockIndex(device);
  if (index < 0 || index >= 2) return 0;
  const auto &image = rootIdentities[index];
  if (strnlen(image.identity, sizeof(image.identity)) != 32) return 0;
  SHA384_CTX context;
  SHA384_Init(&context);
  SHA384_Update(&context, image.identity, 32);
  SHA384_Update(&context, &image.bytes, sizeof(image.bytes));
  SHA384_Update(&context, uuid, strlen(uuid) + 1);
  SHA384_Final(digest, &context);
  return 1;
}

extern "C" int native_root_block_authorized(dev_t device)
{
  return rootBlockIndex(device) == 0;
}

extern "C" int native_root_block_cache_authorized(dev_t device)
{
  return rootBlockIndex(device) == 1;
}

static int rootBlockVolume(const char *uuid, dev_t *device, char *path, size_t size, int index)
{
  if (observationSeconds && !observationCall) {
    observationCall = thread_call_allocate(observationExpired, nullptr);
    if (!observationCall) panic("RootStage: observation timer allocation failed");
    uint64_t deadline;
    clock_interval_to_deadline(observationSeconds, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(observationCall, deadline);
    uint32_t follow = 0;
    if (rootObserver && PE_parse_boot_argn("boot-ui-follow", &follow, sizeof(follow)) && follow == 1) {
      thread_t worker;
      if (kernel_thread_start(rootObservationWorker, nullptr, &worker) != KERN_SUCCESS)
        panic("RootStage: UI observation worker creation failed");
      thread_deallocate(worker);
    }
  }
  IORegistryIterator *iterator = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
  if (!iterator) return 0;
  bool found = false;
  while (IORegistryEntry *entry = iterator->getNextObject()) {
    OSString *identity = OSDynamicCast(OSString, entry->getProperty("UUID"));
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid)) continue;
    OSString *name = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));
    OSNumber *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
    OSNumber *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
    if (!name || !majorNumber || !minorNumber) continue;
    dev_t candidate = makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue());
    if (rootBlockIndex(candidate) != index) continue;
    if (snprintf(path, size, "/dev/%s", name->getCStringNoCopy()) >= (int)size) break;
    *device = candidate;
    found = true;
    break;
  }
  iterator->release();
  return found;
}

extern "C" int native_root_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
  return rootBlockVolume(uuid, device, path, size, 1);
}

extern "C" int native_root_block_data_seed(dev_t *device, char *path, size_t size,
    uint64_t *base, uint64_t *bytes, uint8_t digest[32])
{
  if (!rootBlockVolume(dataSeedUUID, device, path, size, 2)) return 0;
  *base = 0;
  *bytes = dataSeedBytes;
  memcpy(digest, dataSeedHash, 32);
  return 1;
}
