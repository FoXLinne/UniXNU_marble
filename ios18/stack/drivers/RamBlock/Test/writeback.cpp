// Host test of the write-back mode (PersistOverlay.h dirty/flush) through DirectDisk.h: random page writes, flushes and power losses,
// against a medium whose volatile cache may have written any subset of the unflushed writes when the power goes. After each power loss
// the reopened overlay may name only pages whose content was written to them, no older than at the last completed flush, and every
// page written before that flush must be present. Failures injected inside flush must keep the same guarantees.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../DirectDisk.h"
using namespace persist;

struct Medium {
  std::vector<uint8_t> durable;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending;   // the volatile cache, in write order
  unsigned writes = 0, syncs = 0, failWriteAt = ~0u, failSyncAt = ~0u;
  explicit Medium(uint64_t blocks) : durable(blocks * kBlock, 0xA5) {}      // never-written medium holds junk
  bool read(uint64_t b, uint32_t n, void *d) {
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t *src = &durable[(b + i) * kBlock];
      for (auto &p : pending) if (p.first == b + i) src = p.second.data();   // the device reads back its own cache
      memcpy((uint8_t *) d + (uint64_t) i * kBlock, src, kBlock);
    }
    return true;
  }
  bool write(uint64_t b, uint32_t n, const void *s) {
    if (writes++ == failWriteAt) return false;
    for (uint32_t i = 0; i < n; ++i)
      pending.push_back({b + i, std::vector<uint8_t>((const uint8_t *) s + (uint64_t) i * kBlock, (const uint8_t *) s + (uint64_t) (i + 1) * kBlock)});
    return true;
  }
  bool sync() {
    if (syncs++ == failSyncAt) return false;
    for (auto &p : pending) memcpy(&durable[p.first * kBlock], p.second.data(), kBlock);
    pending.clear();
    return true;
  }
  void powerLoss() {                                     // any subset of the cache reached the medium, in write order
    for (auto &p : pending) if (rand() % 3 == 0) memcpy(&durable[p.first * kBlock], p.second.data(), kBlock);
    pending.clear();
  }
};
struct Buffer {
  std::vector<uint8_t> &data;
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, &data[at], n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(&data[at], p, n); return n; }
};

static const uint64_t kPages = 32768 + 1000;            // two bitmap blocks
static uint64_t pickPage() { uint64_t k = (uint64_t) rand() % 1200; return k < 600 ? k : 32700 + (k - 600); }   // both blocks
static uint32_t tagOf(const uint8_t *page) { uint32_t t; memcpy(&t, page, 4); for (int k = 4; k < 4096; k += 4) assert(!memcmp(page, page + k, 4)); return t; }

int main() {
  srand(11);
  RamSparseDisk seed = {}; seed.bytes = kPages * 4096;
  std::vector<uint8_t *> seedPages(kPages, nullptr); seed.pages = seedPages.data();   // an all-zero seed: tag 0
  uint8_t digest[32] = {5};
  Medium m(windowBlocks(kPages));
  std::vector<uint8_t> bits(bitmapBlocks(kPages) * kBlock), dirty(bitmapBlocks(kPages)), stage(8 * kBlock), io(8 * kBlock);
  Overlay ov; ov.dirty = dirty.data();
  assert(ov.open(m, kPages, digest, false, bits.data(), true) == Open::Fresh && ov.dirtyBlocks == 0);
  m.sync();
  // current: the tag a read returns now; flushed: the tag as of the last completed flush (0 = seed). Tags only grow.
  std::vector<uint32_t> current(kPages, 0), flushed(kPages, 0);
  std::vector<std::vector<uint32_t>> written(kPages);    // every tag ever written to the page since the last check
  uint32_t nextTag = 1;
  unsigned losses = 0, flushes = 0, failedFlushes = 0;
  bool stopped = false;                                   // a failed flush: the caller stops writing until the next boot
  for (int op = 0; op < 60000; ++op) {
    int what = rand() % 100;
    if (what < 80 && !stopped) {                          // a run of 1..8 whole pages
      uint64_t first = pickPage(), n = 1 + (uint64_t) rand() % 8;
      if (first + n > kPages) n = kPages - first;
      for (uint64_t k = 0; k < n; ++k) {
        uint32_t t = nextTag++;
        for (int b = 0; b < 4096; b += 4) memcpy(&io[k * 4096 + b], &t, 4);
        current[first + k] = t; written[first + k].push_back(t);
      }
      Buffer buf{io}; bool err = false;
      assert(directDiskIO(seed, ov, m, stage.data(), 8, true, first * 4096, n * 4096, buf, &err) == n * 4096 && !err);
    } else if (what < 93 && !stopped) {                   // the disk's synchronize, sometimes failing inside
      int fail = rand() % 10;
      if (fail == 0) m.failSyncAt = m.syncs + (unsigned) (rand() % 2);
      if (fail == 1) m.failWriteAt = m.writes;
      uint64_t blocks = 0;
      if (ov.flush(m, &blocks)) { flushed = current; ++flushes; }
      else { stopped = true; ++failedFlushes; }
      m.failSyncAt = m.failWriteAt = ~0u;
    } else if (what < 100) {                              // power loss, reboot, check, carry on from what the medium holds
      m.powerLoss(); ++losses;
      Overlay r; std::vector<uint8_t> b2(bits.size());
      assert(r.open(m, kPages, digest, true, b2.data()) == Open::Resumed);
      for (uint64_t i = 0; i < kPages; ++i) {
        if (r.has(i)) {
          uint8_t page[4096]; m.read(pageBlock(kPages, i), 1, page);
          uint32_t t = tagOf(page);
          bool ever = false; for (uint32_t w : written[i]) ever |= w == t;
          assert((ever || t == flushed[i]) && t >= flushed[i]);   // a real write to this page, not older than the last flush
          current[i] = t;
        } else {
          assert(flushed[i] == 0);                        // nothing flushed is lost
          current[i] = 0;
        }
      }
      flushed = current;
      for (auto &w : written) w.clear();
      ov = Overlay(); ov.dirty = dirty.data();
      assert(ov.open(m, kPages, digest, false, bits.data()) == Open::Resumed && ov.dirtyBlocks == 0);
      stopped = false;
      // Reads through the reopened overlay agree with the check.
      for (int k = 0; k < 50; ++k) { uint64_t i = pickPage(); Buffer buf{io}; bool err = false;
        assert(directDiskIO(seed, ov, m, stage.data(), 8, false, i * 4096, 4096, buf, &err) == 4096 && !err && tagOf(io.data()) == current[i]); }
    }
  }
  assert(losses > 100 && flushes > 1000 && failedFlushes > 10);
  // Write through is unchanged: without `dirty` every write puts its bits on the medium at once, with no flush.
  { Medium t(windowBlocks(kPages)); std::vector<uint8_t> b(bits.size()); Overlay w;
    assert(w.open(t, kPages, digest, false, b.data(), true) == Open::Fresh); t.sync();
    for (int b2 = 0; b2 < 4096; b2 += 4) { uint32_t v = 7; memcpy(&io[b2], &v, 4); }
    assert(w.writePage(t, 32800, io.data()) && w.dirtyBlocks == 0);
    t.sync(); Overlay r; std::vector<uint8_t> b3(bits.size());
    assert(r.open(t, kPages, digest, true, b3.data()) == Open::Resumed && r.has(32800) && r.present == 1); }
  printf("PASS: write back: %u flushes, %u failed flushes, %u power losses; no bit without its page, nothing flushed lost\n",
         flushes, failedFlushes, losses);
  return 0;
}
