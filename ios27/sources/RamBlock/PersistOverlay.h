// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Persistent overlay over a seeded sparse disk. The seed (the packed RAM disk the loader provides) stays the immutable base; the
// pages a running system changes are stored in a window of 4096-byte blocks on some other medium and put back on the next boot.
//
// Window layout (block = 4096 bytes, all integers little endian):
//   block 0                 header: "SHNGDATA", version 1, header bytes 64, page count, SHA-256 of the seed, generation (32 bit), crc32
//   blocks 1 .. B           presence bitmap, one bit per page of the disk, B = ceil(pages / 32768)
//   blocks B+1 .. B+pages   page i at B+1+i, valid only when its bit is set
// Two write modes; in both a bit on the medium never promises a page that did not reach the medium.
// Write through (dirty == nullptr): write() returns only when the blocks are on the medium (the UFS store uses WRITE(10) with FUA). A
// run of pages is written first and the bitmap blocks that gained a bit after it, both before the caller's write completes, so a page
// the system was told is written is never lost to a crash.
// Write back (dirty set): write() may leave the blocks in the medium's volatile cache, as a disk with a write cache does. A run of
// pages is written and its new bits are set in memory only; their bitmap blocks are marked in `dirty`. flush() (the disk's
// synchronize) makes the pages durable (sync), then writes the marked bitmap blocks, then makes those durable (sync). A crash before a
// flush may lose the writes since the previous one, never the order: a page whose bit did not reach the medium is read from the seed.
// An overwrite of a page whose bit is set changes only that page.
// An overlay is only ever applied on top of the exact seed it was made against. A window that holds no overlay of this seed (blank,
// damaged, or made against another seed or size) is made anew only when the caller asks for it (`reset`: the boot menu's Install and
// boot); otherwise it is left as it is (Kept) and the disk runs from the seed alone, so no boot replaces a user's Data by itself.
//
// Ops: bool read(uint64_t block, uint32_t count, void *dst); bool write(uint64_t block, uint32_t count, const void *src);
//      bool sync();   (blocks are relative to the window; sync is only used while the header is made)
#include <stdint.h>
#include <string.h>

namespace persist {
constexpr uint32_t kBlock = 4096;
constexpr uint32_t kHeaderBytes = 64;

inline uint32_t crc32 (const uint8_t *data, size_t bytes)
{
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
inline void put32 (uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t (v >> (8 * i)); }
inline void put64 (uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t (v >> (8 * i)); }
inline uint32_t get32 (const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
inline uint64_t get64 (const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

inline uint64_t bitmapBlocks (uint64_t pages) { return (pages + 8ULL * kBlock - 1) / (8ULL * kBlock); }
inline uint64_t windowBlocks (uint64_t pages) { return 1 + bitmapBlocks (pages) + pages; }
inline uint64_t pageBlock (uint64_t pages, uint64_t index) { return 1 + bitmapBlocks (pages) + index; }

inline void encodeHeader (uint8_t block[kBlock], uint64_t pages, const uint8_t seed[32], uint64_t generation)
{
  memset (block, 0, kBlock);
  memcpy (block, "SHNGDATA", 8);
  put32 (block + 8, 1);
  put32 (block + 12, kHeaderBytes);
  put64 (block + 16, pages);
  memcpy (block + 24, seed, 32);
  put32 (block + 56, uint32_t (generation));
  // The CRC covers the first 60 bytes (the generation included); bytes 60..63 hold it.
  put32 (block + 60, crc32 (block, 60));
}
// 0 valid and for this seed; 1 blank or foreign (no magic); 2 damaged (CRC, version, size); 3 valid but for another seed/size.
inline int checkHeader (const uint8_t block[kBlock], uint64_t pages, const uint8_t seed[32], uint64_t *generation)
{
  if (memcmp (block, "SHNGDATA", 8)) return 1;
  if (get32 (block + 8) != 1 || get32 (block + 12) != kHeaderBytes || get32 (block + 60) != crc32 (block, 60)) return 2;
  if (generation) *generation = get32 (block + 56);
  if (get64 (block + 16) != pages || memcmp (block + 24, seed, 32)) return 3;
  return 0;
}

// Resumed: this seed's overlay, applied. Fresh: made anew (reset asked). Blank: read only, nothing applied. Kept: no overlay of this
// seed and no reset asked, the window untouched. Failed: an I/O or format error.
enum class Open { Resumed, Fresh, Blank, Kept, Failed };

struct Overlay {
  uint64_t pages = 0, generation = 0, present = 0;
  uint8_t *bitmap = nullptr;       // bitmapBlocks(pages) * kBlock bytes, owned by the caller
  uint8_t *dirty = nullptr;        // write back: bitmapBlocks(pages) bytes, owned by the caller, 1 = block gained a bit since the last flush
  uint64_t dirtyBlocks = 0;        // marked entries of `dirty`
  bool readOnly = true, active = false;
  int header = -1;                 // checkHeader result at open

  // Readers may test bits while a writer commits others (commitRun under the caller's lock), so bytes are read and set atomically.
  bool has (uint64_t i) const { return __atomic_load_n (&bitmap[i >> 3], __ATOMIC_ACQUIRE) >> (i & 7) & 1; }

  // `readOnly`: never write; a stale or blank window is reported (Blank) and left alone. Otherwise a window without this seed's
  // overlay is initialised only with `reset` (bitmap first, header last, so a tear leaves no valid header), and kept as it is
  // without (Kept). `reset` also drops this seed's own overlay.
  template <class Ops> Open open (Ops &ops, uint64_t pageCount, const uint8_t seed[32], bool ro, uint8_t *bits, bool reset = false)
  {
    pages = pageCount; bitmap = bits; readOnly = ro; present = 0; active = false;
    uint64_t bb = bitmapBlocks (pages);
    dirtyBlocks = 0;
    if (dirty) memset (dirty, 0, bb);
    uint8_t block[kBlock];
    if (!pages || !ops.read (0, 1, block)) return Open::Failed;
    header = checkHeader (block, pages, seed, &generation);
    if (header == 0 && reset && !ro) header = 3;   // the caller asked to start over: handled like a stale overlay
    if (header == 0) {
      for (uint64_t b = 0; b < bb; ++b) if (!ops.read (1 + b, 1, bitmap + b * kBlock)) return Open::Failed;
      // Bits past the last page must be clear, or the loader would read blocks outside the window.
      for (uint64_t i = pages; i < bb * 8ULL * kBlock; ++i) if (has (i)) return Open::Failed;
      for (uint64_t i = 0; i < pages; ++i) present += has (i);
      active = true;
      return Open::Resumed;
    }
    memset (bitmap, 0, bb * kBlock);
    if (ro) return Open::Blank;
    if (!reset) return Open::Kept;
    // The cleared bitmap goes out in one write (the store splits it into its largest transfers), not one command per block: a 210 GiB
    // disk has 1680 bitmap blocks, and that many forced-unit-access writes held the medium long enough for the root volumes that
    // share it to miss their publication deadline.
    if (!ops.write (1, static_cast<uint32_t> (bb), bitmap)) return Open::Failed;
    if (!ops.sync ()) return Open::Failed;
    generation = (header == 3 || header == 2) ? generation + 1 : 1;
    encodeHeader (block, pages, seed, generation);
    if (!ops.write (0, 1, block) || !ops.sync ()) return Open::Failed;
    active = true;
    return Open::Fresh;
  }

  // Calls load(index, page) for every present page, in order, reading runs of up to maxRun consecutive present pages with one
  // read into scratch (maxRun * kBlock bytes). Stops at the first failure.
  template <class Ops, class Load> bool forEachPresent (Ops &ops, Load &load, uint8_t *scratch, uint32_t maxRun)
  {
    if (!maxRun) return false;
    for (uint64_t i = 0; i < pages;) {
      if (!has (i)) { ++i; continue; }
      uint32_t n = 1;
      while (n < maxRun && i + n < pages && has (i + n)) ++n;
      if (!ops.read (pageBlock (pages, i), n, scratch)) return false;
      for (uint32_t k = 0; k < n; ++k) if (!load (i + k, scratch + (uint64_t) k * kBlock)) return false;
      i += n;
    }
    return true;
  }

  // Writes `count` consecutive pages starting at `index` (count * kBlock bytes at data). Write through: then the bitmap blocks that
  // gained a bit. Write back: those blocks are only marked dirty for flush().
  // On false the run may be partly on the medium but no bit names a page that is not; the caller must stop using the overlay.
  template <class Ops> bool writeRun (Ops &ops, uint64_t index, uint32_t count, const void *data)
  {
    return writePages (ops, index, count, data) && commitRun (ops, index, count);
  }

  // The two halves of writeRun, for callers that run writes concurrently: writePages needs no lock (it only writes the window's
  // page blocks); commitRun, after it, sets the bits and writes or marks their bitmap blocks, and must be serialized by the caller
  // (one commit at a time, and never during flush), so that bitmap blocks reach the medium in the order their bits were set.
  template <class Ops> bool writePages (Ops &ops, uint64_t index, uint32_t count, const void *data)
  {
    if (readOnly || !active || !count || index >= pages || count > pages - index) return false;
    return ops.write (pageBlock (pages, index), count, data);
  }

  template <class Ops> bool commitRun (Ops &ops, uint64_t index, uint32_t count)
  {
    if (readOnly || !active || !count || index >= pages || count > pages - index) return false;
    uint64_t firstBlock = ~0ULL, lastBlock = 0;
    for (uint64_t i = index; i < index + count; ++i) {
      if (has (i)) continue;
      uint64_t b = i / (8ULL * kBlock);
      if (firstBlock == ~0ULL) firstBlock = b;
      lastBlock = b;
    }
    // Set the bits only after the pages are written; in write through the bitmap blocks follow before this call returns.
    for (uint64_t i = index; i < index + count; ++i)
      if (!has (i)) { __atomic_fetch_or (&bitmap[i >> 3], uint8_t (1 << (i & 7)), __ATOMIC_RELEASE); ++present; }
    if (firstBlock == ~0ULL) return true;
    if (dirty) {
      for (uint64_t b = firstBlock; b <= lastBlock; ++b) if (!dirty[b]) { dirty[b] = 1; ++dirtyBlocks; }
      return true;
    }
    for (uint64_t b = firstBlock; b <= lastBlock; ++b) if (!ops.write (1 + b, 1, bitmap + b * kBlock)) return false;
    return true;
  }

  // Write back: the disk's synchronize. The pages written so far reach the medium first, then the bitmap blocks that gained a bit,
  // then those. A crash anywhere in between leaves every bit on the medium naming a page that is there. *blocks: bitmap blocks
  // written. On false the caller must stop writing (the in-memory bitmap may be ahead of the medium).
  template <class Ops> bool flush (Ops &ops, uint64_t *blocks = nullptr)
  {
    if (blocks) *blocks = 0;
    if (readOnly || !active) return true;
    if (!ops.sync ()) return false;
    if (!dirty || !dirtyBlocks) return true;
    const uint64_t bb = bitmapBlocks (pages);
    uint64_t written = 0;
    for (uint64_t b = 0; b < bb; ++b) {
      if (!dirty[b]) continue;
      if (!ops.write (1 + b, 1, bitmap + b * kBlock)) return false;
      ++written;
    }
    if (!ops.sync ()) return false;
    memset (dirty, 0, bb);
    dirtyBlocks = 0;
    if (blocks) *blocks = written;
    return true;
  }
  template <class Ops> bool writePage (Ops &ops, uint64_t index, const void *page) { return writeRun (ops, index, 1, page); }
};
}  // namespace persist
