// Host test of concurrent direct-mode I/O (NativeRamBlock directReadWrite) over DirectDisk.h and PersistOverlay.h, with the same lock
// structure as the kext: reads take no lock; a write holds its page range and the flush lock shared and commits its bits under one
// lock; a flush holds the flush lock exclusive. Writer threads write whole-page runs and partial pages on their own pages (spread over
// both bitmap blocks) and 512-byte slices of shared pages; reader threads read everything; a flusher flushes. Crash snapshots taken
// at random moments (the durable medium plus any subset of the volatile cache, in write order) must reopen to an overlay whose every
// present page holds a complete written page. At the end each own page holds its last write, each shared slice its writer's last value,
// and after a final flush a crash loses nothing. Run in both write-back and write-through modes. Build with -fsanitize=thread too.
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>
#include <vector>
#include "../DirectDisk.h"
using namespace persist;

struct Medium {
  std::mutex m;
  std::vector<uint8_t> durable;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending;
  bool cache;
  Medium(uint64_t blocks, bool cache) : durable(blocks * kBlock, 0xA5), cache(cache) {}
  bool read(uint64_t b, uint32_t n, void *d) {
    std::lock_guard<std::mutex> g(m);
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t *src = &durable[(b + i) * kBlock];
      for (auto &p : pending) if (p.first == b + i) src = p.second.data();
      memcpy((uint8_t *) d + (uint64_t) i * kBlock, src, kBlock);
    }
    return true;
  }
  bool write(uint64_t b, uint32_t n, const void *s, bool fua) {
    std::lock_guard<std::mutex> g(m);
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t *page = (const uint8_t *) s + (uint64_t) i * kBlock;
      if (!cache || fua) {
        for (auto it = pending.begin(); it != pending.end();) it = it->first == b + i ? pending.erase(it) : it + 1;
        memcpy(&durable[(b + i) * kBlock], page, kBlock);
      } else pending.push_back({b + i, std::vector<uint8_t>(page, page + kBlock)});
    }
    return true;
  }
  bool sync() {
    std::lock_guard<std::mutex> g(m);
    for (auto &p : pending) memcpy(&durable[p.first * kBlock], p.second.data(), kBlock);
    pending.clear();
    return true;
  }
  std::vector<uint8_t> crash(std::mt19937 &rng) {      // what a power loss now would leave
    std::lock_guard<std::mutex> g(m);
    std::vector<uint8_t> image = durable;
    for (auto &p : pending) if (rng() % 2) memcpy(&image[p.first * kBlock], p.second.data(), kBlock);
    return image;
  }
};
struct Store {
  Medium *medium; uint64_t base; bool cached;
  bool read(uint64_t b, uint32_t n, void *d) { return medium->read(base + b, n, d); }
  bool write(uint64_t b, uint32_t n, const void *s) { return medium->write(base + b, n, s, !cached); }
  bool sync() { return medium->sync(); }
};
struct Image {
  std::vector<uint8_t> &data;
  bool read(uint64_t b, uint32_t n, void *d) { memcpy(d, &data[b * kBlock], (uint64_t) n * kBlock); return true; }
  bool write(uint64_t, uint32_t, const void *) { return false; }
  bool sync() { return false; }
};
struct Seed {
  uint64_t bytes;
  const uint8_t *find(uint64_t) const { return nullptr; }   // an all-zero seed
};
struct Buffer {
  uint8_t *data;
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, data + at, n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(data + at, p, n); return n; }
};

static const uint64_t kPages = 32768 + 1200;           // two bitmap blocks
#ifndef ROUNDS
#define ROUNDS 4000
#endif
static const unsigned kWriters = 4, kReaders = 3, kOwn = 96, kShared = 4, kRounds = ROUNDS;
static const uint32_t kStage = 8;

// A page written by this test: 4-byte tag repeated, or (shared pages) eight 512-byte slices each holding its own tag.
static bool wholeTag(const uint8_t *page, uint32_t *tag) {
  memcpy(tag, page, 4);
  for (int k = 4; k < 4096; k += 4) if (memcmp(page, page + k, 4)) return false;
  return true;
}

struct Disk {
  Medium medium;
  Overlay ov;
  std::vector<uint8_t> bits, dirty;
  bool writeBack;
  std::mutex bitLock, rangeLock, flushMutex;
  std::shared_mutex flushRw;
  std::condition_variable rangeFree;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  bool unflushed = false;
  Seed seed{kPages * 4096};
  explicit Disk(bool wb) : medium(windowBlocks(kPages), wb), bits(bitmapBlocks(kPages) * kBlock), dirty(bitmapBlocks(kPages)), writeBack(wb) {
    Store store{&medium, 0, false};
    uint8_t hash[32] = {1};
    if (wb) ov.dirty = dirty.data();
    assert(ov.open(store, kPages, hash, false, bits.data(), true) == Open::Fresh);
  }
  struct Commit {
    Disk *d; Store &s;
#ifdef MUTATE_ORDER
    bool operator()(uint64_t index, uint32_t count) { return d->ov.commitRun(s, index, count); }   // wrong on purpose: unserialized
#else
    bool operator()(uint64_t index, uint32_t count) { std::lock_guard<std::mutex> g(d->bitLock); return d->ov.commitRun(s, index, count); }
#endif
  };
  bool io(bool write, uint64_t offset, uint64_t length, uint8_t *data) {
    std::vector<uint8_t> stage(kStage * kBlock);
    const uint64_t first = offset / 4096, last = (offset + length - 1) / 4096;
    if (write) {
      std::unique_lock<std::mutex> g(rangeLock);
      rangeFree.wait(g, [&] { for (auto &r : ranges) if (r.first <= last && first <= r.second) return false; return true; });
      ranges.push_back({first, last});
    }
    bool ok;
    {
      std::shared_lock<std::shared_mutex> shared(flushRw, std::defer_lock);
      if (write) shared.lock();
      Store store{&medium, 0, writeBack};
      Commit commit{this, store};
      Buffer buf{data};
      bool storeError = false;
      ok = directDiskIO(seed, ov, store, stage.data(), kStage, write, offset, length, buf, &storeError, commit) == length && !storeError;
      if (write && writeBack) { std::lock_guard<std::mutex> g(bitLock); unflushed = true; }
    }
    if (write) {
      std::lock_guard<std::mutex> g(rangeLock);
      for (auto it = ranges.begin(); it != ranges.end(); ++it) if (it->first == first && it->second == last) { ranges.erase(it); break; }
      rangeFree.notify_all();
    }
    return ok;
  }
  bool flush() {
    if (!writeBack) return true;
    std::lock_guard<std::mutex> m(flushMutex);
#ifdef MUTATE_FLUSH
    std::shared_lock<std::shared_mutex> x(flushRw);       // wrong on purpose: writes may commit bits during the flush
#else
    std::unique_lock<std::shared_mutex> x(flushRw);
#endif
    if (!unflushed) return true;
    Store store{&medium, 0, true};
    bool ok = ov.flush(store);
    if (ok) { std::lock_guard<std::mutex> g(bitLock); unflushed = false; }
    return ok;
  }
};

// Every present page of a crash image must be a complete page this test wrote (never the medium's junk).
static void checkCrash(std::vector<uint8_t> &image) {
  Image ops{image};
  std::vector<uint8_t> bits(bitmapBlocks(kPages) * kBlock);
  Overlay ov;
  uint8_t hash[32] = {1};
  assert(ov.open(ops, kPages, hash, true, bits.data()) == Open::Resumed);
  for (uint64_t i = 0; i < kPages; ++i) {
    if (!ov.has(i)) continue;
    const uint8_t *page = &image[pageBlock(kPages, i) * kBlock];
    uint32_t tag;
    if (!wholeTag(page, &tag)) {                         // a shared page: slices
      for (int s = 0; s < 8; ++s) for (int k = 4; k < 512; k += 4) assert(!memcmp(page + s * 512, page + s * 512 + k, 4));
    } else assert(tag != 0xA5A5A5A5u);
  }
}

static uint64_t ownPage(unsigned w, unsigned k) { return k % 2 ? 32700 + w * kOwn + k : 100 + w * kOwn + k; }   // both bitmap blocks
static uint64_t sharedPage(unsigned k) { return 5000 + k; }

static void run(bool writeBack) {
  Disk disk(writeBack);
  std::vector<std::vector<uint32_t>> own(kWriters, std::vector<uint32_t>(kOwn, 0));
  std::vector<std::vector<uint32_t>> slice(kWriters, std::vector<uint32_t>(kShared, 0));
  std::atomic<bool> done{false};
  std::atomic<unsigned> crashes{0};
  std::vector<std::thread> threads;
  for (unsigned w = 0; w < kWriters; ++w) threads.emplace_back([&, w] {
    std::mt19937 rng(w + 1);
    std::vector<uint8_t> data(kStage * 2 * kBlock);
    for (unsigned r = 0; r < kRounds; ++r) {
      const unsigned kind = rng() % 4;
      const uint32_t tag = (w + 1) << 24 | r;
      if (kind == 0) {                                   // a 512-byte slice of a shared page: read, merge, write back
        const unsigned k = rng() % kShared;
        uint32_t value = tag;
        for (int i = 0; i < 512; i += 4) memcpy(&data[i], &value, 4);
        assert(disk.io(true, sharedPage(k) * 4096 + w * 512, 512, data.data()));
        slice[w][k] = value;
      } else if (kind == 1) {                            // a partial own page
        const unsigned k = rng() % kOwn;
        uint64_t page = ownPage(w, k);
        std::vector<uint8_t> whole(4096);
        assert(disk.io(false, page * 4096, 4096, whole.data()));
        for (int i = 0; i < 4096; i += 4) memcpy(&whole[i], &tag, 4);
        const uint64_t skip = (rng() % 8) * 512, n = 512 * (1 + rng() % (8 - skip / 512));
        // the rest of the page keeps its old content: write the whole new tag only when it covers the page
        if (skip == 0 && n == 4096) { assert(disk.io(true, page * 4096, 4096, whole.data())); own[w][k] = tag; }
        else {
          std::vector<uint8_t> current(4096);
          assert(disk.io(false, page * 4096, 4096, current.data()));
          assert(disk.io(true, page * 4096, 4096, whole.data()));   // keep pages whole-tagged: overwrite, then a same-tag partial
          assert(disk.io(true, page * 4096 + skip, n, whole.data() + skip));
          own[w][k] = tag;
        }
      } else {                                           // a run of whole own pages (consecutive indices k, k+2, ... are not adjacent,
        const unsigned k = rng() % kOwn;                 // so write one page, or a run on the low block where pages are adjacent)
        uint64_t page = ownPage(w, k);
        unsigned run = 1;
        if (k % 2 == 0) { run = 1; }
        for (int i = 0; i < 4096; i += 4) memcpy(&data[i], &tag, 4);
        assert(disk.io(true, page * 4096, 4096ull * run, data.data()));
        own[w][k] = tag;
      }
    }
  });
  for (unsigned r = 0; r < kReaders; ++r) threads.emplace_back([&, r] {
    std::mt19937 rng(100 + r);
    std::vector<uint8_t> data(16 * kBlock);
    while (!done) {
      const uint64_t page = rng() % 2 ? 100 + rng() % (kWriters * kOwn) : 32700 + rng() % (kWriters * kOwn + 64);
      const uint32_t n = 1 + rng() % 16;
      if (page + n > kPages) continue;
      assert(disk.io(false, page * 4096, 4096ull * n, data.data()));
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t tag;
        assert(wholeTag(&data[i * kBlock], &tag));       // a page never reads torn or as junk
        assert(tag != 0xA5A5A5A5u);
      }
    }
  });
  threads.emplace_back([&] {                           // the flusher
    std::mt19937 rng(7);
    while (!done) {
      assert(disk.flush());
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
    }
  });
  threads.emplace_back([&] {                           // power losses, at any moment (also inside a flush)
    std::mt19937 rng(8);
    while (!done) { auto image = disk.medium.crash(rng); checkCrash(image); ++crashes; }
  });
  for (unsigned w = 0; w < kWriters; ++w) threads[w].join();
  done = true;
  for (unsigned t = kWriters; t < threads.size(); ++t) threads[t].join();
  // Final content.
  std::vector<uint8_t> page(4096);
  for (unsigned w = 0; w < kWriters; ++w)
    for (unsigned k = 0; k < kOwn; ++k) {
      assert(disk.io(false, ownPage(w, k) * 4096, 4096, page.data()));
      uint32_t tag;
      assert(wholeTag(page.data(), &tag) && tag == own[w][k]);
    }
  for (unsigned k = 0; k < kShared; ++k) {
    assert(disk.io(false, sharedPage(k) * 4096, 4096, page.data()));
    for (unsigned w = 0; w < kWriters; ++w) { uint32_t v; memcpy(&v, &page[w * 512], 4); assert(v == slice[w][k]); }
  }
  // After a flush a power loss keeps everything.
  assert(disk.flush());
  std::mt19937 rng(9);
  auto image = disk.medium.crash(rng);
  checkCrash(image);
  Image ops{image};
  std::vector<uint8_t> bits(bitmapBlocks(kPages) * kBlock);
  Overlay ov;
  uint8_t hash[32] = {1};
  assert(ov.open(ops, kPages, hash, true, bits.data()) == Open::Resumed);
  for (unsigned w = 0; w < kWriters; ++w)
    for (unsigned k = 0; k < kOwn; ++k) {
      if (!own[w][k]) continue;
      assert(ov.has(ownPage(w, k)));
      uint32_t tag;
      assert(wholeTag(&image[pageBlock(kPages, ownPage(w, k)) * kBlock], &tag) && tag == own[w][k]);
    }
  printf("concurrent %s: %u writers x %u rounds, %u readers, %u crash snapshots: ok\n", writeBack ? "write back" : "write through",
         kWriters, kRounds, kReaders, crashes.load());
}

int main() {
  run(true);
  run(false);
  return 0;
}
