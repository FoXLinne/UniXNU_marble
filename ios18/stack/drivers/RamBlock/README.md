# RamBlock

RAM-backed block device and sparse seed storage.

used for the RAM-backed Data volume and the optional persistent overlay.

Build and interface details: [documentation](https://unixnu.com/drivers/ram-block). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

## Direct mode

With the persistent store open read-write, changed pages live only in the store
window (`PersistOverlay.h`) and the seed answers for the rest (`DirectDisk.h`).
Requests run concurrently: reads take no lock; a write holds its page range and
the flush lock shared and commits its bitmap bits under one lock; a flush holds
the flush lock exclusive, so every bit it writes names a page its first sync made
durable. Staging buffers come from a pool of eight.

The store runs write back when the board offers cached writes, unless
`sheng-data-fua=1`: a synchronize, or the idle timer 5 s after a write, makes the
pages and then their bitmap blocks durable. A synchronize with no write since
the last flush sends no command.

`Test/concurrent.cpp` runs readers, writers, a flusher and power-loss snapshots
with the same lock structure in write-back and write-through modes; build it
with `-fsanitize=thread` as well. `Test/writeback.cpp` covers flush failures.
