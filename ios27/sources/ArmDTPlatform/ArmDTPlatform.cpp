// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Generic AArch64 Darwin platform expert for a supplied Apple-format device
// tree. It derives from XNU's real IODTPlatformExpert and relies on the
// platform's own DT root name ("device-tree") for matching. No board name,
// SoC address or vendor identity is compiled into this source.
//
// The DT must contain the nodes XNU's IODT platform path consumes:
//   /chosen with a CHRP NVRAM proxy (nvram-bank-size, nvram-proxy-data)
//   /options, /cpus, /arm-io and /defaults
// The NVRAM proxy is required so the real IODTNVRAM is published before any
// security initialization. This driver does not fabricate NVRAM or suppress
// a missing one; absent inputs make configure() fail.

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <pexpert/pexpert.h>

// Observation counters describe actual published nubs, not started drivers.
extern "C" {
volatile uint32_t armdt_platform_configured = 0;
volatile uint32_t armdt_platform_child_count = 0;
volatile uint32_t armdt_platform_cpu_count = 0;
volatile uint32_t armdt_platform_bus_count = 0;
volatile uint32_t armdt_platform_defaults_count = 0;
volatile uint32_t armdt_platform_nvram_proxy = 0;
volatile uint32_t armdt_platform_device_count = 0;
}

class ArmDTPlatform : public IODTPlatformExpert {
  OSDeclareDefaultStructors (ArmDTPlatform)
public:
  bool configure (IOService *provider) override;
  const char *deleteList (void) override;
  const char *excludeList (void) override;
  bool getModelName (char *name, int maxLength) override;
  long getGMTTimeOfDay (void) override;
  void setGMTTimeOfDay (long secs) override;
private:
  bool hasNVRAMProxy (IOService *provider);
  bool createDeviceNubs (IOService *provider);
};

OSDefineMetaClassAndStructors (ArmDTPlatform, IODTPlatformExpert)

// The calendar at boot. XNU sets it from getGMTTimeOfDay() before any process runs. The base class answers 0, so on a platform
// without a clock driver the system started in 1970 and jumped half a century when the network time arrived. Software that keeps
// periodic timers by the calendar then replays the whole gap: on sheng (macOS 27.3) rpcsvchost's timer loop kept a core busy for
// minutes after every boot, and every periodic background activity was overdue at once.
// A board's clock driver registers with armdt_platform_clock_register: a read function (seconds since 1970, 0 on success) and a
// set function that learns the time the system sets; either may be null. Whatever the clock says, the calendar never starts before the
// kernel it runs was built (the date in the kernel's version string, less a day for its time zone): without a clock the later
// correction is then the time since that build, not the time since 1970.
extern "C" {
extern const char version[];
volatile uint64_t armdt_platform_clock_floor = 0;   // the build date found in the version string, for the boot log
static int (*volatile gClockRead) (uint64_t *);
static void (*volatile gClockSet) (uint64_t);
void
armdt_platform_clock_register (int (*read) (uint64_t *seconds), void (*set) (uint64_t seconds))
{
  __atomic_store_n (&gClockSet, set, __ATOMIC_RELEASE);
  __atomic_store_n (&gClockRead, read, __ATOMIC_RELEASE);
}
}

namespace {
// "Darwin Kernel Version 27.3.0: Fri Oct 30 02:01:13 PDT 2026; root:..." -> seconds since 1970 of that date, 0 if not in this form.
uint64_t
versionSeconds (const char *text)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *p = text;
  while (*p && !(p[0] == ':' && p[1] == ' ')) ++p;
  if (!*p) return 0;
  p += 2;
  while (*p && *p != ' ') ++p;                      // weekday
  if (*p != ' ') return 0;
  ++p;
  unsigned month = 12;
  for (unsigned i = 0; i < 12; ++i) {
    if (p[0] == months[3 * i] && p[1] == months[3 * i + 1] && p[2] == months[3 * i + 2]) month = i;
  }
  if (month == 12 || p[3] != ' ') return 0;
  p += 4;
  auto number = [&p] (unsigned limit) -> unsigned {
    unsigned value = 0, digits = 0;
    while (*p == ' ') ++p;
    while (*p >= '0' && *p <= '9' && digits < 4) {value = value * 10 + (unsigned) (*p - '0'); ++p; ++digits;}
    return digits && value <= limit ? value : limit + 1;
  };
  const unsigned day = number (31);
  const unsigned hour = number (23);
  if (*p != ':') return 0;
  ++p;
  const unsigned minute = number (59);
  if (*p != ':') return 0;
  ++p;
  const unsigned second = number (60);
  while (*p == ' ') ++p;
  while (*p && *p != ' ') ++p;                      // time zone name
  const unsigned year = number (9999);
  if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60 || year < 2020 || year > 9999) return 0;
  // Days since 1970-01-01 of a civil date (months counted from March).
  const unsigned y = month < 2 ? year - 1 : year, m = month < 2 ? month + 10 : month - 2;
  const uint64_t era = y / 400, yoe = y - era * 400, doy = (153 * m + 2) / 5 + day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const uint64_t days = era * 146097 + doe - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}
}

long
ArmDTPlatform::getGMTTimeOfDay (void)
{
  uint64_t built = versionSeconds (version);
  if (built > 86400) built -= 86400;                // the version's time is local to where it was built
  armdt_platform_clock_floor = built;
  uint64_t now = 0;
  int (*read) (uint64_t *) = __atomic_load_n (&gClockRead, __ATOMIC_ACQUIRE);
  if (read != nullptr && read (&now) == 0 && now >= built) {
    return (long) now;
  }
  return (long) built;
}

void
ArmDTPlatform::setGMTTimeOfDay (long secs)
{
  void (*set) (uint64_t) = __atomic_load_n (&gClockSet, __ATOMIC_ACQUIRE);
  if (set != nullptr && secs > 0) {
    set ((uint64_t) secs);
  }
}

bool ArmDTPlatform::getModelName (char *name, int maxLength)
{
  auto *provider = getProvider ();
  auto *model = provider ? OSDynamicCast (OSData, provider->getProperty ("ui-hardware-model")) : nullptr;
  if (model == nullptr) return IODTPlatformExpert::getModelName (name, maxLength);
  const auto *value = static_cast<const char *> (model->getBytesNoCopy ());
  if (maxLength <= 0 || !value || strnlen (value, model->getLength ()) == model->getLength ()) return false;
  return strlcpy (name, value, static_cast<size_t> (maxLength)) < static_cast<size_t> (maxLength);
}

// Serialized empty name lists: XNU's processTopLevel() treats a null pointer
// as "every entry" (remove every child), not as "nothing". An empty list
// Serialized empty name lists: XNU's processTopLevel() treats a null pointer
// as "every entry" (remove every child), not as "nothing". An empty list
// genuinely selects no entry, so the supplied device tree is preserved.
const char *ArmDTPlatform::deleteList (void)
{
  return "()";
}

const char *ArmDTPlatform::excludeList (void)
{
  return "()";
}

bool ArmDTPlatform::hasNVRAMProxy (IOService *provider)
{
  // RELEASE IODTNVRAM removes nvram-proxy-data after parsing its image.
  // Recognize that initialized service before looking for an unused input.
  IORegistryEntry *options = provider->childFromPath ("options", gIODTPlane);
  bool initialized = options != nullptr && OSDynamicCast (IODTNVRAM, options) != nullptr;
  if (options != nullptr) options->release ();
  if (initialized) return true;
  IORegistryEntry *chosen = provider->childFromPath ("chosen", gIODTPlane);
  if (chosen == nullptr) {
    return false;
  }
  OSData *bankSize = OSDynamicCast (OSData, chosen->getProperty ("nvram-bank-size"));
  OSData *proxyData = OSDynamicCast (OSData, chosen->getProperty ("nvram-proxy-data"));
  bool valid = (bankSize != nullptr) && (bankSize->getLength () == sizeof (uint32_t)) &&
               (proxyData != nullptr) && (proxyData->getLength () != 0);
  chosen->release ();
  return valid;
}

bool ArmDTPlatform::createDeviceNubs (IOService *provider)
{
  IORegistryEntry *bus = provider->childFromPath ("arm-io", gIODTPlane);
  if (bus == nullptr) {
    return false;
  }
  // IODTPlatformExpert's IOPlatformDevice nubs resolve resources through
  // their provider's IOPlatformExpert::getNubResources. They must therefore
  // be attached to this platform expert, not to the arm-io IOPlatformDevice
  // (ArmVirtPlatform uses the same parent relationship). The device-tree
  // parent remains arm-io, so reg/ranges resolution still follows the bus.
  // Publish every real arm-io child, not only the first pattern match. The
  // GIC lives here, and the UFS host/PHY nodes must be visible to their native
  // drivers too. processTopLevel() still uses deleteList()/excludeList().
  OSIterator *iterator = bus->getChildIterator (gIODTPlane);
  bus->release ();
  if (iterator == nullptr) {
    return false;
  }
  uint32_t count = 0;
  IORegistryEntry *entry;
  while ((entry = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != nullptr) {
    IOService *nub = createNub (entry);
    if (nub == nullptr) {
      continue;
    }
    if (!nub->attach (this)) {
      nub->release ();
      continue;
    }
    nub->registerService ();
    nub->release ();
    count++;
  }
  iterator->release ();
  armdt_platform_device_count = count;
  return true;
}

static void platformStageReset (void)
{
  // PSCI SYSTEM_RESET via the same SMC conduit as the entry reset probe.
  register uint64_t x0 __asm__ ("x0") = 0x84000009;
  register uint64_t x1 __asm__ ("x1") = 0;
  register uint64_t x2 __asm__ ("x2") = 0;
  register uint64_t x3 __asm__ ("x3") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "memory");
  for (;;) { __asm__ volatile ("wfe"); }
}

bool ArmDTPlatform::configure (IOService *provider)
{
  IORegistryEntry *cpus = provider->childFromPath ("cpus", gIODTPlane);
  IORegistryEntry *bus = provider->childFromPath ("arm-io", gIODTPlane);
  IORegistryEntry *defaults = provider->childFromPath ("defaults", gIODTPlane);
  IORegistryEntry *options = provider->childFromPath ("options", gIODTPlane);
  bool structure = (cpus != nullptr) && (bus != nullptr) &&
                   (defaults != nullptr) && (options != nullptr);
  if (cpus != nullptr) { cpus->release (); }
  if (bus != nullptr) { bus->release (); }
  if (defaults != nullptr) { defaults->release (); }
  if (options != nullptr) { options->release (); }
  if (!structure) {
    IOLog ("ArmDTPlatform: required /cpus, /arm-io, /defaults or /options node is absent\n");
    return false;
  }
  if (!hasNVRAMProxy (provider)) {
    IOLog ("ArmDTPlatform: /chosen does not supply a usable CHRP NVRAM proxy\n");
    return false;
  }
  unsigned platformStage = 0;
  PE_parse_boot_argn ("platform-stage", &platformStage, sizeof (platformStage));
  if (platformStage == 1) {
    platformStageReset ();
  }

  // The inherited implementation publishes real IODTNVRAM from /chosen,
  // then attaches the CPU and top-level nubs. No identity or resource is
  // invented here; failure is returned instead of replaced with a stub.
  if (!IODTPlatformExpert::configure (provider)) {
    return false;
  }
  // XNU's IOKitInitializeTime waits up to 30 s for the "IORTC" resource before it sets the calendar. On a platform whose device
  // tree describes no real-time clock (/arm-io/rtc) no driver will ever publish it, and every boot stalled for nearly the whole
  // timeout (sheng: 28 s of silence before storage starts, macOS run43/run44/run47). Such a platform says at once that there is
  // nothing to wait for. The calendar starts from getGMTTimeOfDay() below and the time daemon sets it.
  IORegistryEntry *rtc = provider->childFromPath ("arm-io/rtc", gIODTPlane);
  if (rtc != nullptr) {
    rtc->release ();
  } else {
    publishResource ("IORTC");
  }
  if (platformStage == 2) {
    platformStageReset ();
  }

  OSIterator *iterator = getChildIterator (gIOServicePlane);
  if (iterator == nullptr) {
    return false;
  }
  uint32_t count = 0;
  uint32_t cpuCount = 0;
  uint32_t busCount = 0;
  uint32_t defaultsCount = 0;
  IORegistryEntry *child;
  while ((child = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != nullptr) {
    count++;
    OSData *type = OSDynamicCast (OSData, child->getProperty ("device_type"));
    if ((type != nullptr) && (type->getLength () == 4) &&
        (memcmp (type->getBytesNoCopy (), "cpu", 4) == 0)) {
      cpuCount++;
    }
    if (strcmp (child->getName (), "arm-io") == 0) {
      busCount++;
    }
    if (strcmp (child->getName (), "defaults") == 0) {
      defaultsCount++;
    }
  }
  iterator->release ();
  armdt_platform_child_count = count;
  armdt_platform_cpu_count = cpuCount;
  armdt_platform_bus_count = busCount;
  armdt_platform_defaults_count = defaultsCount;
  armdt_platform_nvram_proxy = 1;
  armdt_platform_configured = 1;
  // Publish the bus child devices (interrupt controller, VirtIO MMIO, ...)
  // as real IOPlatformDevice nubs. No driver is started here.
  if (!createDeviceNubs (provider)) {
    return false;
  }
  if (platformStage == 3) {
    platformStageReset ();
  }
  // Explicit hardware A/B diagnostic: reach the end of the real platform
  // configuration and panic before publishing the service. The payload's
  // panic-screen probe makes this observable without a serial console.
  unsigned platformProbe = 0;
  PE_parse_boot_argn ("platform-panic", &platformProbe, sizeof (platformProbe));
  if (platformProbe != 0 || platformStage == 4) {
    platformStageReset ();
  }
  registerService ();
  if (platformStage == 5) {
    platformStageReset ();
  }
  return true;
}
