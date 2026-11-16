// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WiFi module's network side: the Ethernet interface the system routes through, the registry entries by which the wireless daemon and
// the network configuration find a wireless interface, and the Apple80211 requests (ioctls on that interface) the daemon uses to scan,
// join and read state. Nothing here talks to the radio: it calls the module's station interface (nw_abi.h) and turns what it answers into
// the structures the system's wireless framework reads.
//
// Layouts below are those the framework reads and writes; where a field's meaning is not known it is left zero. Every request is written
// to the log (type, direction, length) and a request that is not handled is refused, so which ones the framework needs shows in the log.
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/kpi_mbuf.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <stdarg.h>
#include "MarbleWifi.h"
#include "MarbleWifiScanPolicy.h"
#include "nw_abi.h"
#include "nw_control_endpoint.h"

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void marble_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_MarbleWifiNet.cpp");
extern "C" void native_wifi_logf(int level, const char *fmt, ...);
extern "C" void marble_wifi_wake(void);   // WifiProbe.cpp: the WiFi thread sends the queued frame now

#define SIOCSA80211 0x802869c8UL   // _IOW('i', 200, struct apple80211req)
#define SIOCGA80211 0xc02869c9UL   // _IOWR('i', 201, struct apple80211req)

// (the header declares it without C linkage; the symbol is the C one)
extern "C" int sheng_kev_post(struct kev_msg *) __asm__("_kev_post_msg");

namespace {

struct apple80211req {
  char req_if_name[16];
  int32_t req_type;
  int32_t req_val;
  uint32_t req_len;
  user_addr_t req_data __attribute__((aligned(8)));
};

// request types
enum {
  kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10, kScanResult = 11,
  kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20, kAssociateResult = 21,
  kDisassociate = 22, kIbssMode = 24, kHostApMode = 25, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43, kHardwareVersion = 44, kCountryCode = 51,
  kRadioInfo = 54, kScanReqMultiple = 86, kScanCacheClear = 90, kCurrentNetwork = 103, kRssiBounds = 106, kChannelsInfo = 207, kHwSupportedChannels = 254, kEventFilter = 564,
};
// events
enum { kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10, kEvRsnHandshakeDone = 20, kEvRssiChanged = 39 };
// results of an association, as the framework numbers them
enum { kAssocPending = 0, kAssocOk = 1, kAssocFailed = 2, kAssocTimeout = 9, kAssocBadKey = 21 };

// 22H384 __dispatchAsyncAssocEvent reads ten 44-byte records, result at
// offset 8 and the extended reason at 0x1c8. BSSID starts at record offset 16.
struct assoc_attempt18 {
  uint32_t reserved[2], result, reserved1;
  uint8_t bssid[6], reserved2[10];
  uint32_t details[3];
};
struct assoc_event18 { assoc_attempt18 attempts[10]; uint32_t tail[5]; };
static_assert(sizeof(assoc_attempt18) == 44 && sizeof(assoc_event18) == 460,
              "22H384 association event");

// The iOS 27 adapter retains the observed iOS 18 format for older consumers.
struct assoc_event27 { uint8_t attempts[10][52]; uint32_t reserved[2], reason, result, has_result; };
static_assert(sizeof(assoc_event27) == 540 && __builtin_offsetof(assoc_event27, result) == 0x214 &&
              __builtin_offsetof(assoc_event27, has_result) == 0x218, "iOS27 association event");

struct channel { uint32_t version, channel, flags; };
// 24B119 IO80211 __getScanData: count at0x44, 400 channel records at0x48.
struct scan_channels27 { uint8_t prefix[0x44]; uint32_t count; channel list[400]; };
static_assert(sizeof(scan_channels27) == 0x1308, "24B119 scan channel prefix");
struct channels_list { uint32_t version, count; channel list[401]; uint32_t unused; };           // 4824 bytes
// 24B119 IO80211 Apple80211CopyValue(207) 0x28c7b6eac..0x28c7b70fc: count at 0x0c and
// per-channel columns; wifid builds the unified auto-join channel list from it.
struct channels_info27 {
  uint32_t version; uint8_t reserved0[8]; uint16_t count; uint8_t reserved1[0x32e - 0x0e];
  uint8_t number[400], indoor[400], dfs[400], passive[400], width40[400], width80[400];
  uint8_t reserved2[0x12d0 - 0xc8e]; uint32_t bitmap[400]; uint8_t reserved3[0x1aa0 - 0x1910];
  uint8_t band6[400]; uint8_t reserved4[0x2720 - 0x1c30];
};
static_assert(sizeof(channels_info27) == 0x2720 && __builtin_offsetof(channels_info27, count) == 0x0c &&
              __builtin_offsetof(channels_info27, number) == 0x32e && __builtin_offsetof(channels_info27, indoor) == 0x4be &&
              __builtin_offsetof(channels_info27, dfs) == 0x64e && __builtin_offsetof(channels_info27, passive) == 0x7de &&
              __builtin_offsetof(channels_info27, width40) == 0x96e && __builtin_offsetof(channels_info27, width80) == 0xafe &&
              __builtin_offsetof(channels_info27, bitmap) == 0x12d0 && __builtin_offsetof(channels_info27, band6) == 0x1aa0,
              "24B119 channels info");
struct scan_result {                                                                              // 24B119: 2280 bytes
  uint32_t version; channel ch; uint16_t noise_known; int16_t noise, snr, rssi; uint16_t beacon_interval, capability; uint8_t bssid[6];
  uint8_t rate_count, unused0; uint32_t rates[15]; uint8_t ssid_length, ssid[32], unused1[3]; uint32_t age_ms; uint8_t flags, unused2;
  uint16_t elements_length; uint8_t elements[1024];
  uint8_t reserved[0x8d0 - 0x48c]; uint64_t timestamp_ns; uint8_t tail[0x10];
};
struct assoc_parameters {
  uint32_t ssid_length; uint8_t ssid[32]; uint8_t bssid[6], unused[2];
  uint32_t key_version, key_length, key_cipher; uint16_t key_flags, key_index; uint8_t key[64];
};
struct assoc_request {                                                                            // legacy: 592 bytes
  uint32_t version; uint16_t mode, auth_lower, auth_upper, unused;
  assoc_parameters network; uint8_t rest[0x250 - 0x88];
};
// 22H384 __performAssociation issues request type 20 with 0x384 bytes;
// authentication uses 32-bit fields and the network starts at 0x10.
struct assoc_request18 {                                                                          // 900 bytes
  uint32_t version; uint16_t mode, unused; uint32_t auth_lower, auth_upper;
  assoc_parameters network; uint8_t rest[0x384 - 0x8c];
};
struct assoc_request27 {
  uint32_t version; uint16_t mode, unused; uint32_t auth_lower, auth_upper;
  assoc_parameters network; uint8_t rest[0x290 - 0x8c];
};
static_assert(sizeof(assoc_request27) == 656 && __builtin_offsetof(assoc_request27, network) == 0x10,
              "iOS27 association request");

struct link_event { uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4]; uint8_t involuntary, unused2[3]; uint32_t reason, subreason; uint8_t unused3[4]; };
static_assert(sizeof(scan_result) == 0x8e8 && __builtin_offsetof(scan_result, ssid_length) == 0x60 && __builtin_offsetof(scan_result, age_ms) == 0x84 &&
              __builtin_offsetof(scan_result, elements) == 0x8c &&
              __builtin_offsetof(scan_result, timestamp_ns) == 0x8d0, "24B119 scan result layout");
static_assert(sizeof(assoc_parameters) == 0x7c && __builtin_offsetof(assoc_parameters, key) == 0x3c &&
              __builtin_offsetof(assoc_parameters, key_length) == 0x30 &&
              __builtin_offsetof(assoc_parameters, key_cipher) == 0x34, "association network prefix");
static_assert(sizeof(assoc_request) == 592 && __builtin_offsetof(assoc_request, network) == 0x0c, "legacy association layout");
static_assert(sizeof(assoc_request18) == 900 && __builtin_offsetof(assoc_request18, network) == 0x10, "22H384 association layout");
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");

ifnet_t gIf;
uint8_t gMac[6];
lck_grp_t *gGrp;
lck_mtx_t *gLock;
bool gPower = true;
bool gLinkSuppressed;   // hide the old station snapshot while an explicit disconnect drains
bool gKeyed;   // the last join used a passphrase
uint32_t gRxFrames, gRxErrors; int gRxLastError;
uint32_t gRxNoBuffer, gRxChains, gRxLongest;   // frames dropped for want of a buffer, frames that took a chain, the longest frame
mbuf_t gRxHead, gRxTail;
mbuf_t gRxSpare;
uint32_t gRxBulkAllocations, gRxAllocRetries;
uint32_t gRxPendingPackets, gRxPendingBytes, gRxBatches, gRxLargestBatch;
int gAssocResult = kAssocPending;
unsigned gAssocFormat; // 0 legacy, 18:22H384, 27:24A446-compatible
bool gJoinPending;
unsigned gScanCursor, gScanning;
uint64_t gLastScanMs, gLastScanStartMs;
uint64_t wifi_now_ms() { uint64_t ticks, ns; clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks, &ns); return ns / 1000000; }
bool gUserJoin;   // the wireless daemon joined, left or switched the radio itself: the unattended test join steps aside
uint32_t gLogged[256 / 32];

void log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log(int level, const char *fmt, ...) {
  char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  native_wifi_logf(level, "%s", line);
}

// ---- events to the framework: class IEEE 802.11 (6), subclass 1, data starting with the interface name ----
void post_event(uint32_t code, const void *payload, size_t length) {
  char name[16] = "en0";
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = 6; msg.kev_subclass = 1; msg.event_code = code;
  msg.dv[0].data_length = sizeof name; msg.dv[0].data_ptr = name;
  msg.dv[1].data_length = static_cast<uint32_t>(length); msg.dv[1].data_ptr = const_cast<void *>(payload);
  const int error = sheng_kev_post(&msg);
  if (code != kEvScanDone || error) log(6, "wifi-net: event %u bytes %u -> %d", code, static_cast<unsigned>(length), error);
}
// link state for the network configuration: the interface's flags and the stack's own link events
void post_link(bool up) {
  if (!gIf) return;
  // BSD IFF_RUNNING means driver resources allocated. Carrier is conveyed by
  // the link event below; an enabled, unassociated station remains running.
  ifnet_set_flags(gIf, gPower ? IFF_RUNNING : 0, IFF_RUNNING);
  nw_wifi_status current{}; nw_wifi_status_get(&current);
  const int quality = current.signal_dbm >= -65 ? 100 : current.signal_dbm >= -75 ? 50 : current.signal_dbm >= -85 ? 20 : 10;
  ifnet_set_link_quality(gIf, up ? quality : -2);
  struct { struct net_event_data data; } payload; memset(&payload, 0, sizeof payload);
  payload.data.if_family = IFNET_FAMILY_ETHERNET; payload.data.if_unit = 0; snprintf(payload.data.if_name, sizeof payload.data.if_name, "en");
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = KEV_NETWORK_CLASS; msg.kev_subclass = KEV_DL_SUBCLASS; msg.event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
  msg.dv[0].data_length = sizeof payload; msg.dv[0].data_ptr = &payload;
  sheng_kev_post(&msg);
}

// 24B119 CoreWiFi __setupEventHandlersWithInterfaceName 0x246a09230 accepts
// APPLE80211_M_BSSID_CHANGED only with 28 bytes; the change reason is at 0x18.
struct bssid_event27 { uint8_t bssid[6]; uint8_t reserved[0x12]; uint32_t reason; };
static_assert(sizeof(bssid_event27) == 0x1c && __builtin_offsetof(bssid_event27, reason) == 0x18, "24B119 BSSID event");
void post_bssid_changed(const uint8_t *bssid) {
  bssid_event27 event{}; if (bssid) memcpy(event.bssid, bssid, sizeof event.bssid);
  post_event(kEvBssidChanged, &event, sizeof event);
}
void post_link_down(bool involuntary, int reason) {
  post_link(false);
  link_event ev{}; ev.down = 1; ev.involuntary = involuntary; ev.reason = static_cast<uint32_t>(reason);
  post_event(kEvLinkChanged, &ev, sizeof ev);
  post_event(kEvSsidChanged, nullptr, 0); post_bssid_changed(nullptr);
}
void post_association_result() {
  if (gAssocFormat == 27) {
    assoc_event27 event{}; event.result = gAssocResult; event.has_result = 1;
    nw_wifi_status current; nw_wifi_status_get(&current); memcpy(event.attempts[0], current.bssid, 6);
    post_event(kEvAssocDone, &event, sizeof event);
  } else if (gAssocFormat == 18) {
    assoc_event18 event{}; event.attempts[0].result = gAssocResult;
    nw_wifi_status current; nw_wifi_status_get(&current);
    memcpy(event.attempts[0].bssid, current.bssid, 6);
    post_event(kEvAssocDone, &event, sizeof event);
  } else post_event(kEvAssocDone, nullptr, 0);
}
void disconnect_user() {
  const bool pending = __atomic_exchange_n(&gJoinPending, false, __ATOMIC_ACQ_REL);
  gUserJoin = true; gLinkSuppressed = true; gAssocResult = kAssocFailed;
  nw_wifi_disconnect();
  marble_wifi_command_wake();
  // A requested disconnect suppresses the station's link_lost callback. Notify
  // both BSD and Apple80211 here, before asynchronous firmware teardown completes.
  log(6, "wifi-net: explicit link down");
  post_link_down(false, 0);
  if (pending) post_association_result();
}
int drain_disconnect() {
  disconnect_user(); marble_wifi_command_wake();
  // Ioctls run on the requesting process, independently of the station worker.
  // Do not replace its one-slot DISCONNECT request with CONNECT before teardown.
  for (unsigned waited = 0; waited < 2000; waited += 10) {
    nw_wifi_status current; nw_wifi_status_get(&current);
    if (current.state < NW_STA_AUTH) return 0;
    IOSleep(10);
  }
  log(4, "wifi-net: disconnect did not drain within 2000 ms");
  return ETIMEDOUT;
}
void visible_status(nw_wifi_status &st) {
  nw_wifi_status_get(&st);
  if (!gPower || gLinkSuppressed) {
    st.state = NW_STA_IDLE; st.ssid_len = 0; memset(st.ssid, 0, sizeof st.ssid);
    memset(st.bssid, 0, sizeof st.bssid); st.freq_mhz = 0; st.signal_dbm = 0;
  }
}

// ---- what the module tells us ----
void on_scan_done() {
  gLastScanMs = wifi_now_ms();
  __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE); gScanCursor = 0;
  if (gPower) post_event(kEvScanDone, nullptr, 0);
}
// A scan answered from the cache finishes after the time a short scan takes: answered at once, the daemon asks again straight away.
thread_call_t gCachedScan, gCacheOnlyScan;
// Cache-only Apple80211 requests complete independently of a radio scan.
void cache_only_scan_done(thread_call_param_t, thread_call_param_t) {
  gScanCursor = 0;
  if (gPower) post_event(kEvScanDone, nullptr, 0);
}
void cached_scan_done(thread_call_param_t, thread_call_param_t) {
  if (__atomic_exchange_n(&gScanning, 0u, __ATOMIC_ACQ_REL) && gPower)
    post_event(kEvScanDone, nullptr, 0);
}
void on_connect_done(int result) {
  if (!gPower || gLinkSuppressed) {
    log(6, "wifi-net: ignore join completion after explicit disconnect: %d", result);
    return;
  }
  nw_wifi_status snapshot; nw_wifi_status_get(&snapshot);
  if (result == NW_RES_OK && snapshot.state != NW_STA_UP) {
    log(6, "wifi-net: ignore authorization without an active link"); return;
  }
  gAssocResult = result == NW_RES_OK ? kAssocOk : result == NW_RES_BAD_KEY ? kAssocBadKey : result == NW_RES_TIMEOUT ? kAssocTimeout : kAssocFailed;
  const bool pending = __atomic_exchange_n(&gJoinPending, false, __ATOMIC_ACQ_REL);
  log(6, "wifi-net: join finished: module result %d", result);
  post_link(result == NW_RES_OK);
  if (pending) post_association_result();
  if (result == NW_RES_OK) {
    // Port authorization means the supplicant completed key installation.
    if (gKeyed) post_event(kEvRsnHandshakeDone, nullptr, 0);
    nw_wifi_status st; nw_wifi_status_get(&st);
    post_event(kEvSsidChanged, nullptr, 0); post_bssid_changed(st.bssid);
    link_event ev; memset(&ev, 0, sizeof ev); ev.rssi = st.signal_dbm; ev.noise = static_cast<uint16_t>(-95); ev.snr = st.signal_dbm > -95 ? st.signal_dbm + 95 : 0;
    post_event(kEvLinkChanged, &ev, sizeof ev);
  }
}
void on_link_lost(int reason) {
  log(6, "wifi-net: link lost: reason %d", reason);
  gAssocResult = kAssocFailed;
  post_link_down(true, reason);
}
void on_link_update(const nw_wifi_status *status) {
  if (!gPower || gLinkSuppressed || status->state != NW_STA_UP || status->signal_dbm >= 0) return;
  const int quality = status->signal_dbm >= -65 ? 100 : status->signal_dbm >= -75 ? 50 : status->signal_dbm >= -85 ? 20 : 10;
  if (gIf) ifnet_set_link_quality(gIf, quality);
  // 24B119 CoreWiFi accepts RSSI_CHANGED only with a 592-byte payload and drops
  // empty ones after waking wifid. The daemon reads RSSI through its own queries.
}
// What the address configuration exchanges, as it crosses the interface: DHCP messages (type, transaction, offered address) and ARP, the first
// ones of each direction, and per-kind counts for the traffic line.
struct Kinds { uint32_t arp, ip4, dhcp, ip6, other; } gKindsIn, gKindsOut;
void note_frame(const uint8_t *e, size_t n, bool out) {
  Kinds &k = out ? gKindsOut : gKindsIn; const unsigned type = n >= 14 ? (e[12] << 8 | e[13]) : 0; const char *dir = out ? "out" : "in";
  if (type == 0x0806) {
    k.arp++; static unsigned shown[2];
    if (n >= 42 && shown[out]++ < 12) log(6, "wifi-net: arp %s op %u %u.%u.%u.%u asks %u.%u.%u.%u", dir, e[21], e[28], e[29], e[30], e[31], e[38], e[39], e[40], e[41]);
    return; }
  if (type == 0x86dd) { k.ip6++; return; }
  if (type != 0x0800 || n < 34) { k.other++; return; }
  k.ip4++; const uint8_t *ip = e + 14; const unsigned ihl = (ip[0] & 15) * 4;
  if (ip[9] != 17 || n < 14 + ihl + 8) return;
  const uint8_t *udp = ip + ihl; const unsigned sp = udp[0] << 8 | udp[1], dp = udp[2] << 8 | udp[3];
  if (!((sp == 67 || sp == 68) && (dp == 67 || dp == 68))) return;
  k.dhcp++; const uint8_t *b = udp + 8; const size_t bl = n - (b - e); unsigned msg = 0;
  for (size_t o = 240; bl >= 240 && o + 2 <= bl && b[o] != 255; o += b[o] ? 2 + b[o + 1] : 1) if (b[o] == 53 && o + 2 < bl) { msg = b[o + 2]; break; }
  static unsigned shown[2]; if (shown[out]++ < 24 && bl >= 240)
    log(6, "wifi-net: dhcp %s type %u xid %02x%02x%02x%02x flags %02x%02x to %02x:%02x:%02x:%02x:%02x:%02x yiaddr %u.%u.%u.%u", dir, msg, b[4], b[5], b[6], b[7], b[10], b[11],
        e[0], e[1], e[2], e[3], e[4], e[5], b[16], b[17], b[18], b[19]);
  // An offer or acknowledgement: the network settings the server hands out (mask, router, name servers, lease), to tell a server that
  // gives no IPv4 name server from a client that drops one.
  if (out || (msg != 2 && msg != 5)) return;
  char opts[124]; int at = 0;   // fits the 160-byte log line
  for (size_t o = 240; o + 2 <= bl && b[o] != 255 && at >= 0 && at < static_cast<int>(sizeof opts) - 24; o += b[o] ? 2 + b[o + 1] : 1) {
    if (!b[o]) continue;
    const unsigned code = b[o], len = b[o + 1]; const uint8_t *v = b + o + 2;
    if (o + 2 + len > bl) break;
    if ((code == 1 || code == 3 || code == 6 || code == 54) && len >= 4)
      for (unsigned i = 0; i + 4 <= len && at < static_cast<int>(sizeof opts) - 24; i += 4)
        at += snprintf(opts + at, sizeof opts - at, " %u=%u.%u.%u.%u", code, v[i], v[i + 1], v[i + 2], v[i + 3]);
    else if (code == 51 && len == 4) at += snprintf(opts + at, sizeof opts - at, " 51=%u", v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
    else at += snprintf(opts + at, sizeof opts - at, " %u", code);
  }
  log(6, "wifi-net: dhcp %s type %u options%s", dir, msg, opts);
}
void release_rx_spares() {
  while (gRxSpare) {
    mbuf_t packet = gRxSpare; gRxSpare = mbuf_nextpkt(packet);
    mbuf_setnextpkt(packet, nullptr); mbuf_freem(packet);
  }
}
void flush_rx() {
  // A NAPI batch owns at most 16 spare 2 KiB packets; none survives its flush.
  release_rx_spares();
  if (!gRxHead) return;
  mbuf_t packets = gRxHead; const uint32_t count = gRxPendingPackets, bytes = gRxPendingBytes;
  gRxHead = gRxTail = nullptr; gRxPendingPackets = gRxPendingBytes = 0;
  struct ifnet_stat_increment_param inc{}; inc.packets_in = count; inc.bytes_in = bytes;
  const errno_t error = ifnet_input(gIf, packets, &inc);
  gRxBatches++; if (count > gRxLargestBatch) gRxLargestBatch = count;
  if (error) { gRxErrors += count; gRxLastError = error; } else gRxFrames += count;
}
mbuf_t allocate_rx(size_t length) {
  if (length <= 2048) {
    if (!gRxSpare) {
      unsigned chunks = 1;
      if (!mbuf_allocpacket_list(16, MBUF_DONTWAIT, 2048, &chunks, &gRxSpare)) gRxBulkAllocations++;
    }
    if (gRxSpare) {
      mbuf_t packet = gRxSpare; gRxSpare = mbuf_nextpkt(packet);
      mbuf_setnextpkt(packet, nullptr); return packet;
    }
  }
  mbuf_t packet = nullptr;
  if (mbuf_allocpacket(MBUF_DONTWAIT, length, nullptr, &packet)) return nullptr;
  return packet;
}
void note_packet(void *context, size_t length, nw_packet_reader reader, bool out) {
  uint8_t header[64]; const size_t peek = length < sizeof(header) ? length : sizeof(header);
  if (reader(context, 0, header, peek)) return;
  bool dhcp = false;
  if (peek >= 42 && header[12] == 8 && header[13] == 0 && header[23] == 17) {
    const unsigned offset = 14 + (header[14] & 15) * 4;
    if (offset + 4 <= peek) {
      const unsigned source = header[offset] << 8 | header[offset + 1], destination = header[offset + 2] << 8 | header[offset + 3];
      dhcp = (source == 67 || source == 68) && (destination == 67 || destination == 68);
    }
  }
  if (dhcp && length <= 2000) {
    uint8_t frame[2000]; if (!reader(context, 0, frame, length)) note_frame(frame, length, out);
  } else note_frame(header, peek, out);
}
void on_rx_packet(void *context, size_t length, nw_packet_reader reader) {
  if (!gIf || length < 14) return;
  note_packet(context, length, reader, false);
  // Two bytes ahead of the Ethernet header put the IP header on a 4-byte boundary. The packet may come back as a chain (a frame larger
  // than one cluster, or no large cluster free): each buffer gets only what fits in it. Setting the whole length on the first one (as
  // before) let the stack read past that buffer's end (i469: a data abort in m_copydata reading across a page).
  enum : size_t { Align = 2 };
  mbuf_t m = allocate_rx(length + Align);
  if (!m && gRxHead) {
    // Hand already received packets to the input thread before retrying once.
    flush_rx(); gRxAllocRetries++; m = allocate_rx(length + Align);
  }
  if (!m) { gRxNoBuffer++; return; }
  if (length > gRxLongest) gRxLongest = static_cast<uint32_t>(length);
  if (mbuf_next(m)) gRxChains++;
  size_t done = 0; bool first = true;
  for (mbuf_t n = m; n; n = mbuf_next(n)) {
    size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
    if (first) { if (room < Align + 14) break; mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0); room -= Align; }
    const size_t take = length - done < room ? length - done : room;
    if (reader(context, done, mbuf_data(n), take)) { done = 0; break; }
    mbuf_setlen(n, take); done += take; first = false;
  }
  if (done != length) { gRxNoBuffer++; mbuf_freem(m); return; }
  mbuf_pkthdr_setlen(m, length); mbuf_pkthdr_setrcvif(m, gIf);
  // configd resolves the DHCP router through BPF. Deliver the full Ethernet
  // frame before removing its header for the protocol input path.
  bpf_tap_in(gIf, DLT_EN10MB, m, nullptr, 0);
  mbuf_pkthdr_setheader(m, mbuf_data(m)); mbuf_adj(m, 14);   // the stack wants the Ethernet header apart from the payload
  mbuf_setnextpkt(m, nullptr);
  if (gRxTail) mbuf_setnextpkt(gRxTail, m); else gRxHead = m;
  gRxTail = m; gRxPendingPackets++; gRxPendingBytes += static_cast<uint32_t>(length);
  if (gRxPendingPackets >= 64) flush_rx();
}
int flat_read(void *context, size_t offset, void *destination, size_t length) { memcpy(destination, static_cast<uint8_t *>(context) + offset, length); return 0; }
void on_rx(const uint8_t *eth, size_t length) { on_rx_packet(const_cast<uint8_t *>(eth), length, flat_read); }
const struct nw_wifi_events gEvents = { on_scan_done, on_connect_done, on_link_lost, on_rx, flush_rx, on_rx_packet, on_link_update };

// ---- the interface ----
int mbuf_read(void *context, size_t offset, void *destination, size_t length) { return mbuf_copydata(static_cast<mbuf_t>(context), offset, length, destination); }
void mbuf_release(void *context) { mbuf_freem(static_cast<mbuf_t>(context)); }
errno_t if_output(ifnet_t, mbuf_t m) {
  size_t length = mbuf_pkthdr_len(m);
  if (length >= 14 && length <= 2000) {
    note_packet(m, length, mbuf_read, true);
    bpf_tap_out(gIf, DLT_EN10MB, m, nullptr, 0);
    if (nw_wifi_tx_packet(m, length, mbuf_read, mbuf_release)) log(6, "wifi-net: a frame was dropped (queue full)");
    else {ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(length), 0); marble_wifi_wake(); return 0;}
  }
  mbuf_freem(m);
  return 0;
}
void if_event_stub(ifnet_t, const struct kev_msg *) {}

// ---- Apple80211 ----
void fill_channel(channel &c, uint32_t mhz) {
  c.version = 1;
  if (mhz >= 5000) { c.channel = (mhz - 5000) / 5; c.flags = 0x2 | 0x10; } else { c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5; c.flags = 0x2 | 0x8; }
}
void fill_result(scan_result &r, const nw_bss_info &b) {
  memset(&r, 0, sizeof r); r.version = 1; fill_channel(r.ch, b.freq_mhz); r.noise_known = 1; r.noise = -95; r.rssi = b.signal_dbm; r.snr = b.signal_dbm + 95;
  r.beacon_interval = b.beacon_int; r.capability = b.caps; memcpy(r.bssid, b.bssid, 6); r.ssid_length = b.ssid_len; memcpy(r.ssid, b.ssid, 32);
  // IO80211 __addScanResultToList exports AGE from 0x84 and the 64-bit
  // TIMESTAMP from 0x8d0. CoreWiFi converts TIMESTAMP nanoseconds to milliseconds for freshness.
  const uint64_t now = wifi_now_ms();
  r.age_ms = b.age_ms;
  r.timestamp_ns = (now > b.age_ms ? now - b.age_ms : 0) * 1000000ULL;
  r.elements_length = b.ie_len > sizeof r.elements ? sizeof r.elements : b.ie_len;
  memcpy(r.elements, b.ie, r.elements_length);
  r.rate_count = 0;
}
int copy_out(const apple80211req &rq, const void *src, size_t size) {
  if (rq.req_len == 0 || !rq.req_data) return EINVAL;
  return copyout(src, rq.req_data, rq.req_len < size ? rq.req_len : size);
}
// A get that gives no buffer takes a single value in req_val; one with a buffer gets {version, value}.
int answer_value(apple80211req &rq, uint32_t value) {
  if (rq.req_len == 0 || !rq.req_data) { rq.req_val = static_cast<int32_t>(value); return 0; }
  const uint32_t w[2] = { 1, value }; return copyout(w, rq.req_data, rq.req_len < sizeof w ? rq.req_len : sizeof w);
}
// The raw bytes, their length in req_len.
int answer_bytes(apple80211req &rq, const void *src, uint32_t size) {
  if (!rq.req_data) return EINVAL;
  const uint32_t n = rq.req_len < size ? rq.req_len : size; rq.req_len = size; return copyout(src, rq.req_data, n);
}
int copy_in(const apple80211req &rq, void *dst, size_t size) {
  if (!rq.req_data) return EINVAL;
  memset(dst, 0, size);
  return copyin(rq.req_data, dst, rq.req_len < size ? rq.req_len : size);
}
int copy_association(const apple80211req &rq, assoc_parameters &out) {
  // Select only an observed wire layout, never infer it from credential contents.
  if (rq.req_len != sizeof(assoc_request) && rq.req_len != sizeof(assoc_request18) && rq.req_len != sizeof(assoc_request27)) {
    log(4, "wifi-net: unsupported association layout: %u bytes", rq.req_len);
    return EINVAL;
  }
  union { assoc_request legacy; assoc_request18 current; } wire;
  int error = copy_in(rq, &wire, sizeof wire);
  if (!error) {
    if (rq.req_len == sizeof(assoc_request18) || rq.req_len == sizeof(assoc_request27)) {
      if (wire.current.version != 1) error = EINVAL;
      else { out = wire.current.network; log(6, "wifi-net: join auth lower %x upper %x", wire.current.auth_lower, wire.current.auth_upper); }
    } else {
      if (wire.legacy.version != 1) error = EINVAL;
      else { out = wire.legacy.network; log(6, "wifi-net: join auth lower %x upper %x", wire.legacy.auth_lower, wire.legacy.auth_upper); }
    }
  }
  memset(&wire, 0, sizeof wire);
  return error;
}
unsigned gResultsShown;
// The regulatory country as the access points announce it (802.11d: the Country element, id 7, of the strongest network that has one), else "XX".
void country(char cc[3]) {
  cc[0] = 'X'; cc[1] = 'X'; cc[2] = 0; int best = -200; nw_bss_info b;
  for (int i = 0, n = nw_wifi_bss_count(); i < n; ++i) {
    if (nw_wifi_bss_get(i, &b) || b.signal_dbm <= best) continue;
    for (unsigned o = 0; o + 2 <= b.ie_len && o + 2 + b.ie[o + 1] <= b.ie_len; o += 2 + b.ie[o + 1])
      if (b.ie[o] == 7 && b.ie[o + 1] >= 2 && b.ie[o + 2] >= 'A' && b.ie[o + 2] <= 'Z' && b.ie[o + 3] >= 'A' && b.ie[o + 3] <= 'Z') {
        cc[0] = static_cast<char>(b.ie[o + 2]); cc[1] = static_cast<char>(b.ie[o + 3]); best = b.signal_dbm; break; }
  }
}
int map_state(unsigned s) { switch (s) { case NW_STA_SCANNING: return 1; case NW_STA_AUTH: return 2; case NW_STA_ASSOC: case NW_STA_KEYS: return 3; case NW_STA_UP: return 4; default: return 0; } }

int apple80211(bool set, apple80211req &rq) {
  nw_wifi_status st; visible_status(st);
  const int type = rq.req_type;
  // Frequent scalar/status requests need no 5 KiB temporary allocation.
  if (!set) switch (type) {
    case kState: return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
    case kPowerSave: { nw_power_save_status ps{}; nw_wifi_power_save_get(&ps); return answer_value(rq, ps.applied); }
    case kOpMode: return answer_value(rq, 1);
    case kRate: return answer_value(rq, st.state == NW_STA_UP ? st.tx_rate_100kbps / 10 : 0);
    case kAssociateResult: return answer_value(rq, static_cast<uint32_t>(gAssocResult));
    case kSsid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.ssid, st.ssid_len);
    case kBssid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.bssid, 6);
    case kRssiBounds: {
      // 24B119 IO80211 request 106: version, signed dBm min/max/autojoin
      // threshold. wifid otherwise keeps its legacy scaled-RSSI default +7.
      const int32_t bounds[4] = {1, -95, -20, -80};
      return copy_out(rq, bounds, sizeof bounds);
    }
    case kChannelsInfo: {
      if (rq.req_len < sizeof(channels_info27) || !rq.req_data) return EINVAL;
      auto *info = static_cast<channels_info27 *>(IOMallocData(sizeof(channels_info27)));
      if (!info) return ENOMEM;
      memset(info, 0, sizeof *info); info->version = 1;
      unsigned n = 0;
      for (int i = 0, count = nw_wifi_channel_count(); i < count && n < 400; i++) {
        nw_channel_info channel{};
        if (nw_wifi_channel_get(static_cast<unsigned>(i), &channel) || (channel.flags & 1) || channel.frequency >= 5925) continue;
        const unsigned number = channel.frequency >= 5000 ? (channel.frequency - 5000) / 5 :
                                channel.frequency == 2484 ? 14 : (channel.frequency - 2407) / 5;
        const bool high = channel.frequency >= 5000;
        // 5 GHz 80 MHz blocks 36-48, 52-64, 100-112, 116-128, 132-144 and 149-161.
        const bool block80 = high && ((number >= 36 && number <= 64) || (number >= 100 && number <= 144) || (number >= 149 && number <= 161));
        info->number[n] = static_cast<uint8_t>(number);
        info->dfs[n] = (channel.flags & 4) != 0;
        info->passive[n] = (channel.flags & 2) != 0;
        info->width40[n] = high ? block80 : number <= 13;
        info->width80[n] = block80;
        n++;
      }
      info->count = static_cast<uint16_t>(n);
      const int error = copyout(info, rq.req_data, sizeof *info);
      IOFreeData(info, sizeof *info);
      return error;
    }
    default: break;
  }
  // (not on the stack: the largest answer is 4824 bytes)
  union Out { uint8_t raw[5120]; channels_list chans; scan_result res; };
  Out *outp = static_cast<Out *>(IOMallocData(sizeof(Out)));
  if (!outp) return ENOMEM;
  memset(outp, 0, sizeof(Out));
  struct Free { Out *p; ~Free() { IOFreeData(p, sizeof(Out)); } } freeOut{outp};
  Out &out = *outp;
  uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
  if (set) {
    switch (type) {
    case kEventFilter: {
      // 24B119 Apple80211FilterMonitoringEvent submits its 33-byte interest
      // bitmap before wifid creates the SCDynamicStore subscription. KEV
      // delivery is shared; each Apple80211 handle filters its own events.
      // Keep driver delivery as a superset for concurrent consumers.
      uint8_t interest[33];
      if (rq.req_len != sizeof interest) return EINVAL;
      return copy_in(rq, interest, sizeof interest);
    }
    case kOpMode: {
      uint32_t mode = static_cast<uint32_t>(rq.req_val);
      if (rq.req_len) { uint32_t input[2]; if (rq.req_len < sizeof input || copy_in(rq, input, sizeof input) || input[0] != 1) return EINVAL; mode = input[1]; }
      // The registered cfg80211 primary interface is an infrastructure STA.
      return mode == 1 ? 0 : EOPNOTSUPP;
    }
    case kIbssMode: case kHostApMode:
      // wifid stops AP/IBSS during station initialization. This board exposes
      // one station vif and has no AP or IBSS instance to tear down.
      return !rq.req_len && rq.req_val == 2 ? 0 : EOPNOTSUPP;
    case kPower: { uint32_t in[6]; if (copy_in(rq, in, sizeof in)) return EFAULT; gUserJoin = true; gPower = in[2] != 0; ifnet_set_flags(gIf, gPower ? IFF_RUNNING : 0, IFF_RUNNING); log(6, "wifi-net: power set %u", in[2]); gLinkSuppressed = !gPower; if (!gPower) { gLastScanMs = gLastScanStartMs = 0; if (gCachedScan) thread_call_cancel(gCachedScan); if (gCacheOnlyScan) thread_call_cancel(gCacheOnlyScan); __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE); disconnect_user(); } nw_wifi_power(gPower); marble_wifi_command_wake(); post_event(kEvPowerChanged, nullptr, 0); return 0; }
    case kScanReq: case kScanReqMultiple: {
      if (!gPower) return ENETDOWN;
      if (gLinkSuppressed) {
        nw_wifi_status current; nw_wifi_status_get(&current);
        if (current.state >= NW_STA_AUTH) return EBUSY; // preserve pending teardown
      }
      log(6, "wifi-net: scan enter op=%d bytes=%u active=%u", type, rq.req_len, __atomic_load_n(&gScanning, __ATOMIC_ACQUIRE));
      uint8_t buf[0x80]; if (copy_in(rq, buf, sizeof buf)) return EFAULT;
      // 24B119 __getScanData places SCAN_TYPE at 0x34. CoreWiFi uses
      // type 3 for cacheOnly and follows an empty/stale cache with type 1/2.
      uint32_t scanType; memcpy(&scanType, buf + 0x34, sizeof scanType);
      if (scanType == 3) {
        if (!gCacheOnlyScan) gCacheOnlyScan = thread_call_allocate(cache_only_scan_done, nullptr);
        if (!gCacheOnlyScan) return ENOMEM;
        gScanCursor = 0; gResultsShown = 24;
        uint64_t at; clock_interval_to_deadline(1, kMillisecondScale, &at);
        thread_call_enter_delayed(gCacheOnlyScan, at);
        log(6, "wifi-net: cache-only scan, %d observations", nw_wifi_bss_count());
        return 0;
      }
      uint32_t ssidLength; memcpy(&ssidLength, buf + 16, 4); const uint8_t *ssid = buf + 20;
      if (ssidLength > 32) return EINVAL;
      uint32_t frequencies[256]; unsigned frequencyCount = 0;
      if (rq.req_len == 0x1598) {
        if (copy_in(rq, out.raw, sizeof(scan_channels27))) return EFAULT;
        const scan_channels27 &request = *reinterpret_cast<const scan_channels27 *>(out.raw);
        if (request.count > 400) return EINVAL;
        for (unsigned i = 0; i < request.count; i++) {
          const channel &requested = request.list[i];
          if (requested.version != 1) return EINVAL;
          for (int c = 0, n = nw_wifi_channel_count(); c < n; c++) {
            nw_channel_info supported{};
            if (nw_wifi_channel_get(static_cast<unsigned>(c), &supported) || (supported.flags & 1) || supported.frequency >= 5925) continue;
            channel expected; fill_channel(expected, supported.frequency);
            if (requested.channel != expected.channel || (requested.flags & (0x18 | 0x2000)) != (expected.flags & (0x18 | 0x2000))) continue;
            unsigned j = 0; while (j < frequencyCount && frequencies[j] != supported.frequency) j++;
            if (j == frequencyCount) {
              if (frequencyCount == 256) return EINVAL;
              frequencies[frequencyCount++] = supported.frequency;
            }
            break;
          }
        }
        if (request.count && !frequencyCount) return EINVAL;
      }
      log(6, "wifi-net: scan request type=%u channels=%u station=%u", scanType, frequencyCount, st.state);
      if (__atomic_exchange_n(&gScanning, 1u, __ATOMIC_ACQ_REL)) { log(6, "wifi-net: scan coalesced"); return 0; }
      const uint64_t now = wifi_now_ms();
      bool freshChannel = false;
      if (!ssidLength && frequencyCount == 1 &&
          marble_wifi_scan_target_cached(now, gLastScanMs, 0)) {
        for (int i = 0, count = nw_wifi_bss_count(); i < count; ++i) {
          nw_bss_info bss{};
          if (!nw_wifi_bss_get(i, &bss) && bss.freq_mhz == frequencies[0] &&
              marble_wifi_scan_target_cached(now, gLastScanMs, bss.age_ms)) {
            freshChannel = true; break;
          }
        }
      }
      if (freshChannel || marble_wifi_scan_cached(now, gLastScanMs, gLastScanStartMs, st.state == NW_STA_UP,
                                 st.state >= NW_STA_AUTH && st.state < NW_STA_UP,
                                 ssidLength != 0 || frequencyCount != 0, nw_wifi_bss_count())) {
        if (!gCachedScan) gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
        if (!gCachedScan) { __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE); return ENOMEM; }
        gScanCursor = 0; gResultsShown = 24;
        uint64_t at; clock_interval_to_deadline(50, kMillisecondScale, &at);
        thread_call_enter_delayed(gCachedScan, at); return 0;
      }
      gScanCursor = 0; gResultsShown = 0;
      int result = nw_wifi_scan_channels(ssidLength ? ssid : nullptr, ssidLength, frequencies, frequencyCount);
      log(6, "wifi-net: scan queued channels=%u result=%d", frequencyCount, result);
      if (result) { __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE); return ENETDOWN; }
      gLastScanStartMs = now;
      marble_wifi_command_wake(); return 0; }
    case kAssociate: {
      assoc_parameters a{};
      struct Clear { assoc_parameters &p; ~Clear() { memset(&p, 0, sizeof p); } } clear{a};
      int error = copy_association(rq, a); if (error) return error;
      if (!a.ssid_length || a.ssid_length > 32 || a.key_length > 64) return EINVAL;
      // The station accepts open networks, WPA passphrases and a 32-byte PMK.
      // Do not silently interpret an unknown cipher as a password or an open network.
      if (a.key_length && !((a.key_cipher == 6 && a.key_length == 32) ||
                            (a.key_cipher == 10 && a.key_length >= 8 && a.key_length <= 63))) return EINVAL;
      const bool hasBssid = (a.bssid[0] | a.bssid[1] | a.bssid[2] | a.bssid[3] | a.bssid[4] | a.bssid[5]) != 0;
      const bool hasKey = a.key_length != 0 && a.key_length <= 64;
      if (!gPower) return ENETDOWN;
      // Consult the physical snapshot too: a just-requested disconnect may not
      // have drained yet, even though getters already report the link as down.
      nw_wifi_status current; nw_wifi_status_get(&current);
      if (current.state >= NW_STA_AUTH) {
        // A matching name does not authenticate the supplied credential.
        // Each explicit request must obtain its own authorization result.
        log(6, "wifi-net: switching network: draining state %u", current.state);
        int error = drain_disconnect(); if (error) return error;
        if (!gPower) return ENETDOWN;
      }
      char pass[65]; memset(pass, 0, sizeof pass); if (hasKey) memcpy(pass, a.key, a.key_length);
      // cipher 6: the daemon already derived the pairwise master key (32 bytes); the module takes it as 64 hex digits
      unsigned keyLength = a.key_length;
      if (hasKey && a.key_cipher == 6 && a.key_length == 32) { static const char hex[] = "0123456789abcdef"; for (int i = 0; i < 32; i++) { pass[2 * i] = hex[a.key[i] >> 4]; pass[2 * i + 1] = hex[a.key[i] & 15]; } keyLength = 64; }
      gAssocResult = kAssocPending; gAssocFormat = rq.req_len == sizeof(assoc_request27) ? 27 : rq.req_len == sizeof(assoc_request18) ? 18 : 0;
      gUserJoin = true; gLinkSuppressed = false;
      __atomic_store_n(&gJoinPending, true, __ATOMIC_RELEASE);
      log(6, "wifi-net: join: layout %u, ssid length %u, bssid given %d, key length %u cipher %u", rq.req_len, a.ssid_length, hasBssid, a.key_length, a.key_cipher);
      gKeyed = hasKey; int r = nw_wifi_connect(a.ssid, a.ssid_length, hasBssid ? a.bssid : nullptr, hasKey ? pass : nullptr, hasKey ? keyLength : 0);
      if (!r) marble_wifi_command_wake();
      else { gAssocResult = kAssocFailed; __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE); }
      memset(pass, 0, sizeof pass); memset(&a, 0, sizeof a);
      return r ? EINVAL : 0; }
    case kDisassociate: case kDeauth: log(6, "wifi-net: leave"); disconnect_user(); return 0;
    case kPowerSave: {
      uint32_t mode = static_cast<uint32_t>(rq.req_val);
      if (rq.req_len) { uint32_t input[2]; if (rq.req_len < sizeof input || copy_in(rq, input, sizeof input)) return EINVAL; if (input[0] != 1) return EINVAL; mode = input[1]; }
      int result = nw_wifi_power_save(mode); if (result) return EINVAL;
      marble_wifi_command_wake(); return 0;
    }
    case kScanCacheClear: gLastScanMs = 0; return 0;
    case kLocale: case kCountryCode: case kAuthType: case kCipherKey: return 0;
    default: return EOPNOTSUPP;
    }
  }
  switch (type) {
  case kPower: w[0] = 1; w[1] = 1; w[2] = gPower && st.state != NW_STA_OFF ? 1 : 0; return copy_out(rq, w, 24);
  case kState: return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
  case kOpMode: return answer_value(rq, 1);   // station
  case kAuthType: w[0] = 1; w[1] = 1; w[2] = gKeyed ? 0x8 : 0; w[3] = 0; return copy_out(rq, w, 16);   // {version, lower: open system, upper: WPA2 personal, extension}
  case kPowerSave: { nw_power_save_status ps{}; nw_wifi_power_save_get(&ps); return answer_value(rq, ps.applied); }
  case kCardCaps: {
    if (!rq.req_len || rq.req_len>sizeof out.raw) return EINVAL;
    // 24B119 Apple80211GetWithIOCTL passes a 23-byte bare bit vector and
    // tests bit n at byte n/8 of that exact pointer (0x28c7be99c..bea28).
    // Unlike the other Apple80211 getters, this payload has no version word.
    const uint32_t caps=nw_wifi_card_capabilities();
    const unsigned bytes=rq.req_len<4 ? rq.req_len : 4;
    for (unsigned i=0;i<bytes;i++) out.raw[i]=static_cast<uint8_t>(caps>>(8*i));
    return answer_bytes(rq,out.raw,rq.req_len); }
  case kPhyMode: { nw_wifi_link_info link{}; nw_wifi_link_info_get(&link);
    w[0] = 1; w[1] = link.supported_modes; w[2] = st.state == NW_STA_UP ? link.active_mode : 0;
    return copy_out(rq, w, 12); }
  case kSsid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.ssid, st.ssid_len);
  case kBssid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.bssid, 6);
  case kChannel: if (st.state < NW_STA_AUTH) return ENOENT; w[0] = 1; fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz); return copy_out(rq, w, 16);
  case kRssi: case kNoise: {   // {version, radios, unit (0: dBm), per chain[4], aggregate, extension per chain[4], its aggregate}
    if (type == kRssi && st.state != NW_STA_UP) return ENOENT;
    const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(st.signal_dbm) : -95);
    w[0] = 1; w[1] = 1; w[2] = 0; for (int i = 3; i < 13; i++) w[i] = v; return copy_out(rq, w, 13 * 4); }
  case kRate: return answer_value(rq, st.state == NW_STA_UP ? st.tx_rate_100kbps / 10 : 0);
  case kTxPower: return answer_value(rq, 15);
  case kDriverVersion: case kHardwareVersion: { const char *v = type == kDriverVersion ? "nativewifi 1" : "WCN6855 hw2.1"; strlcpy(reinterpret_cast<char *>(out.raw), v, 256); return copy_out(rq, out.raw, 256); }
  case kCountryCode: { char cc[3]; country(cc); return answer_bytes(rq, cc, 3); }   // the two letters and a NUL (the daemon asks with a 3-byte buffer)
  case kAssociateResult: return answer_value(rq, static_cast<uint32_t>(gAssocResult));
  case kSupportedChannels: case kHwSupportedChannels: {
    out.chans.version = 1; unsigned n = 0;
    const int count = nw_wifi_channel_count();
    for (int i = 0; i < count && n < 401; i++) {
      nw_channel_info channel{};
      if (nw_wifi_channel_get(static_cast<unsigned>(i), &channel) || (channel.flags & 1) || channel.frequency >= 5925) continue;
      fill_channel(out.chans.list[n], channel.frequency);
      if (!(channel.flags & 2)) out.chans.list[n].flags |= 0x80;
      if (channel.flags & 4) out.chans.list[n].flags |= 0x100;
      n++;
    }
    out.chans.count = n; return copy_out(rq, &out.chans, sizeof out.chans); }
  case kScanResult: {
    nw_bss_info b; const int count = nw_wifi_bss_count();
    if (gScanCursor >= static_cast<unsigned>(count) || nw_wifi_bss_get(static_cast<int>(gScanCursor), &b)) { gScanCursor = 0; return EIO; }   // after the last record
    gScanCursor++; fill_result(out.res, b);
    if (gResultsShown < 24) { ++gResultsShown; log(6, "wifi-net: scan result %u: %u MHz, %d dBm, %.*s", gScanCursor, b.freq_mhz, b.signal_dbm, b.ssid_len, reinterpret_cast<const char *>(b.ssid)); }
    return copy_out(rq, &out.res, sizeof out.res); }
  case kCurrentNetwork: {
    if (st.state < NW_STA_ASSOC) return ENOENT;
    nw_bss_info b; const int count = nw_wifi_bss_count();
    for (int i = 0; i < count; i++) if (!nw_wifi_bss_get(i, &b) && !memcmp(b.bssid, st.bssid, 6)) {
      fill_result(out.res, b); out.res.rssi = st.signal_dbm;
      out.res.snr = st.signal_dbm > -95 && st.signal_dbm < 0 ? st.signal_dbm + 95 : 0;
      return copy_out(rq, &out.res, sizeof out.res);
    }
    return ENOENT; }
  default: return EOPNOTSUPP;
  }
}

errno_t if_ioctl(ifnet_t, unsigned long cmd, void *data) {
  switch (cmd) {
  case SIOCGA80211: case SIOCSA80211: {
    apple80211req *rq = static_cast<apple80211req *>(data);
    const uint32_t len = rq->req_len; const int32_t val = rq->req_val;
    int e = apple80211(cmd == SIOCSA80211, *rq);
    // every request with its answer, for the first few hundred; then a type's first one only
    static unsigned shown; const int type = rq->req_type; const bool first = type >= 0 && type < 256 && !(gLogged[type / 32] & (1u << (type % 32)));
    if (type >= 0 && type < 256) gLogged[type / 32] |= 1u << (type % 32);
    const bool joinState = type == kAssociate || type == kAssociateResult || type == kAuthType || type == kState || type == kDisassociate;
    static uint64_t lastStateLog;
    const uint64_t now = wifi_now_ms();
    const bool reportState = (joinState && (cmd == SIOCSA80211 || e || now - lastStateLog >= 5000)) || ((type == kScanReq || type == kScanReqMultiple) && cmd == SIOCSA80211);
    if (reportState) {
      lastStateLog = now;
      nw_wifi_status current; visible_status(current);
      log(6, "wifi-net: state op %d %c ret %d sta %u pending %u res %d rssi %d", type,
          cmd == SIOCSA80211 ? 'S' : 'G', e, current.state,
          __atomic_load_n(&gJoinPending, __ATOMIC_ACQUIRE), gAssocResult, current.signal_dbm);
    }
    if (shown < 300 || first || reportState) { ++shown; log(6, "wifi-net: request %u: type %d %s len %u val %d -> %d (len %u val %d)", shown, type, cmd == SIOCSA80211 ? "set" : "get", len, val, e, rq->req_len, rq->req_val); }
    return e;
  }
  case SIOCGIFMEDIA: {
    struct ifmediareq *m = static_cast<struct ifmediareq *>(data);
    nw_wifi_status st; visible_status(st);
    // the framework lists wireless interfaces by this media type
    m->ifm_current = IFM_IEEE80211 | IFM_AUTO; m->ifm_mask = 0; m->ifm_status = IFM_AVALID | (st.state == NW_STA_UP ? IFM_ACTIVE : 0);
    m->ifm_active = IFM_IEEE80211 | IFM_AUTO; m->ifm_count = 1;
    return 0;
  }
  case SIOCSIFFLAGS: case SIOCSIFMTU: case SIOCADDMULTI: case SIOCDELMULTI: case SIOCSIFADDR: case SIOCSIFMEDIA: return 0;
  default: { static unsigned said; if (said++ < 16) log(6, "wifi-net: interface request %lx not handled", cmd); return EOPNOTSUPP; }
  }
}

// ---- registry entries (see the top of the file in the build: the classes are registered late) ----
}  // namespace

class IO80211Controller : public IOService { OSDeclareDefaultStructors(IO80211Controller); };
OSDefineMetaClassAndStructors(IO80211Controller, IOService);
class IONetworkInterface : public IOService { OSDeclareDefaultStructors(IONetworkInterface); };
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);
// The system's network configuration and wireless daemon tell a wireless interface by its class name.
class IO80211Interface : public IONetworkInterface {
  OSDeclareDefaultStructors(IO80211Interface);
 public:
  IOReturn newUserClient(task_t, void *, UInt32 type, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE {
    static unsigned said; if (said++ < 4) log(6, "wifi-net: a user client of type %u was asked for: none offered (ioctl path)", static_cast<unsigned>(type));
    return kIOReturnUnsupported;
  }
};
OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface);

// The network configuration's interface namer waits for a service of this class, opens it and only then publishes that IOKit is quiet;
// everything that lists interfaces (the wireless daemon among them) waits for that. Interfaces here are named when they are made, so the
// commands it sends (as properties of its connection) are only logged.
class IONetworkStackUserClient : public IOUserClient {
  OSDeclareDefaultStructors(IONetworkStackUserClient);
 public:
  IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE {
    static unsigned said;
    if (said++ < 16) {
      char text[200] = "?";
      if (OSSerialize *s = OSSerialize::withCapacity(512)) { if (properties && properties->serialize(s)) strlcpy(text, s->text(), sizeof text); s->release(); }
      log(6, "wifi-net: network stack command %s", text);
    }
    return kIOReturnSuccess;
  }
  IOReturn clientClose() APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);
class IONetworkStack : public IOService {
  OSDeclareDefaultStructors(IONetworkStack);
 public:
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *, IOUserClient **handler) APPLE_KEXT_OVERRIDE {
    IONetworkStackUserClient *client = OSTypeAlloc(IONetworkStackUserClient);
    if (!client || !client->initWithTask(task, security, type)) { OSSafeReleaseNULL(client); return kIOReturnNoMemory; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
    log(6, "wifi-net: the network stack was opened (type %u)", static_cast<unsigned>(type));
    *handler = client; return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);

namespace {

IOService *publish(IOService *service, IOService *provider, const char *name) {
  if (!service || !service->attach(provider)) { OSSafeReleaseNULL(service); return nullptr; }
  service->setName(name);
  if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
  service->registerService();
  return service;
}
void publish_registry(const char *ifname) {
  IOService *platform = IOService::getPlatform();
  if (IONetworkStack *stack = OSTypeAlloc(IONetworkStack)) {
    if (!stack->init() || !publish(stack, platform, "IONetworkStack")) log(3, "wifi-net: the network stack service did not start"); else stack->release(); }
  IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
  IO80211Interface *wlan = OSTypeAlloc(IO80211Interface);
  if (!platform || !controller || !wlan || !controller->init() || !wlan->init()) { OSSafeReleaseNULL(controller); OSSafeReleaseNULL(wlan); log(3, "wifi-net: registry objects not made"); return; }
  if (OSData *mac = OSData::withBytes(gMac, 6)) { controller->setProperty("IOMACAddress", mac); mac->release(); }
  controller->setProperty("IOBuiltin", true);
  controller = static_cast<IO80211Controller *>(publish(controller, platform, "MarbleWLANController"));
  if (!controller) { wlan->release(); log(3, "wifi-net: the controller did not start"); return; }
  wlan->setProperty("IOInterfaceName", ifname); wlan->setProperty("BSD Name", ifname); wlan->setProperty("IOInterfaceNamePrefix", "en");
  if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) { wlan->setProperty("IOInterfaceType", n); n->release(); }
  if (OSNumber *n = OSNumber::withNumber(0ULL, 32)) { wlan->setProperty("IOInterfaceUnit", n); n->release(); }
  wlan->setProperty("IOBuiltin", true); wlan->setProperty("IOPrimaryInterface", true); wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
  if (!publish(wlan, controller, "MarbleWLAN")) log(3, "wifi-net: the interface service did not start"); else wlan->release();
  controller->release();
}

}  // namespace

// Called once the module has a station interface and a hardware address.
extern "C" void marble_wifi_net_start(const uint8_t mac[6]) {
  if (gIf) return;
  void *module=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!module) return;
  marble_wifi_net_static_init();
  if (OSMetaClass::postModLoad(module)) return;
  memcpy(gMac, mac, 6);
  gGrp = lck_grp_alloc_init("marble-wifi-net", LCK_GRP_ATTR_NULL); gLock = lck_mtx_alloc_init(gGrp, LCK_ATTR_NULL);
  struct ifnet_init_eparams p; memset(&p, 0, sizeof p);
  p.ver = IFNET_INIT_CURRENT_VERSION; p.len = sizeof p; p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO; p.name = "en"; p.unit = 0; p.family = IFNET_FAMILY_ETHERNET; p.subfamily = IFNET_SUBFAMILY_WIFI; p.type = IFT_ETHER;
  p.output = if_output; p.demux = ether_demux; p.add_proto = ether_add_proto; p.del_proto = ether_del_proto; p.check_multi = ether_check_multi; p.framer = ether_frameout;
  p.ioctl = if_ioctl; p.event = if_event_stub; static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; p.broadcast_addr = bcast; p.broadcast_len = 6;
  // Without NX_NOAUTO the stack puts a compatibility nexus in front of a Wi-Fi "en" interface, and its input no longer goes through the
  // interface's own input thread.
  ifnet_t ifp = nullptr; errno_t e = ifnet_allocate_extended(&p, &ifp);
  if (e || !ifp) { log(3, "wifi-net: ifnet allocation failed: %d", e); return; }
  ifnet_set_mtu(ifp, 1500); ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff); ifnet_set_baudrate(ifp, 54000000ULL);
  ifnet_set_hdrlen(ifp, 14); ifnet_set_addrlen(ifp, 6); ifnet_set_eflags(ifp, IFEF_ENQUEUE_MULTI, IFEF_ENQUEUE_MULTI);
  e = ifnet_attach(ifp, nullptr);
  if (e) { log(3, "wifi-net: ifnet attach failed: %d", e); ifnet_release(ifp); return; }
  ifnet_set_lladdr(ifp, mac, 6);
  // A driver of its own interface attaches the packet filter itself: the DHCP client sends through it while the interface has no address.
  e = bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr); if (e) log(3, "wifi-net: packet filter attach failed: %d", e);
  gIf = ifp; ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
  nw_wifi_set_events(&gEvents);
  publish_registry("en0");
  post_link(false);
  post_event(kEvPowerChanged, nullptr, 0);
  log(6, "wifi-net: en0 is up, hardware address %02x:%02x:%02x:%02x:%02x:%02x, capabilities 0x%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      ifnet_capabilities_enabled(ifp));
}
