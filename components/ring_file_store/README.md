# ring_file_store

Persistent fixed-capacity ring files for ESP-IDF (tested against ESP-IDF
6.0.2, ESP32-S3). Records are caller-defined structs stored in fixed-size
binary slots on any ESP-IDF VFS filesystem (LittleFS, FATFS on SD). One
caller-defined key field inside the record identifies a record. "Oldest"
and "newest" always mean insertion order; keys may arrive out of order and
numeric gaps consume no storage.

The design reuses the fixed-slot storage principle of
[DSMRlogger32](https://github.com/mrWheel/DSMRlogger32) (direct offset seek
into fixed-width history slots) but adds application-defined keys,
insertion order, a versioned binary format and a crash-recoverable commit
protocol. Legacy DSMR CSV import is not part of this component.

```c
#include "ring_file_store.h"

typedef struct
{
  uint64_t timestamp;   //-- key
  uint32_t energyWh;
  uint16_t errorCount;
  uint16_t pad;
} meterRecord_t;

ringFileConfig_t cfg = {0};
cfg.maxRecords = 24;
cfg.schemaId = 0x4D455431;
cfg.schemaVersion = 1;
cfg.recordSize = sizeof(meterRecord_t);
cfg.key = (ringFileKeyDescriptor_t){
  .keyOffset = offsetof(meterRecord_t, timestamp),
  .keyType = RING_FILE_KEY_U64,
  .keySize = sizeof(uint64_t)
};

ringFileHandle_t *ring = NULL;
ESP_ERROR_CHECK(ringFileCreate("/rfs/meter.rfs", &cfg, &ring));

meterRecord_t rec = {0};
rec.timestamp = 1000;
rec.energyWh = 5000;
ESP_ERROR_CHECK(ringFileAdd(ring, &rec));
ringFileClose(ring);
```

## API

| Function | Purpose |
| --- | --- |
| `ringFileCreate` | Create an empty ring exclusively and return it open read-write. Never recreates or truncates (`RING_FILE_STORE_ERR_ALREADY_EXISTS`). |
| `ringFileOpen` | Open existing, read-only or read-write. Validates expected schema. A read-write open may perform recovery writes; a read-only open never writes and returns `RING_FILE_STORE_ERR_RECOVERY_REQUIRED` if recovery is needed. |
| `ringFileClose` | Commit staged data, close, free. A clean handle performs no write and no sync. |
| `ringFileAdd` | Append as newest. Duplicate active key rejected without changes; at capacity exactly the oldest record is evicted. Evicted keys may be reused. |
| `ringFileReadByKey` | Copy active record for a key. |
| `ringFileUpdateByKey` | Replace the complete record, preserve insertion position. Replacement with a differing embedded key: `ESP_ERR_INVALID_ARG`. Identical replacement: no write at all. |
| `ringFileUpsert` | Atomic update-or-add under the handle lock (the hourly-logger pattern). |
| `ringFileReadOldest` / `ringFileReadNewest` | First/last record in insertion order. |
| `ringFileIteratorBegin/Next/Previous` | Caller-owned cursors; start at oldest, newest or an exact key; traverse either direction; `ITER_END` at the logical ends; successful mutations invalidate cursors. |
| `ringFileResize` | Change capacity through a verified candidate file plus an atomic rename switch. Shrink keeps the newest records that fit. Capacity ≥ 1. Persists the new capacity. |
| `ringFileGetInfo` | count, capacity, record size, schema, storage size, pending count, dirty. |
| `ringFileSync` | Commit pending buffered data and synchronize through the backend. |
| `ringFileValidate` | Stand-alone read-only integrity check; distinguishes bad metadata from bad record slots and reports stray resize files. |
| `ringFileBreakStaleLock` | Remove the `<path>.lck` lock file left by an interrupted run. Only for use right after a reset, before the first open, when no live handle can exist. |
| `ringFileClear` | Discard all records (epoch bump in the header). Schema and capacity retained. |

### Result codes

Standard: `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`,
`ESP_ERR_INVALID_STATE`, `ESP_ERR_NOT_FOUND`. Custom, base
`RING_FILE_STORE_ERR_BASE` (0x5600):

| Code | Meaning |
| --- | --- |
| `..._ERR_RING_EMPTY` | readOldest/readNewest/iterator on a ring with no records. |
| `..._ERR_KEY_NOT_FOUND` | No active record carries the key. |
| `..._ERR_DUPLICATE_KEY` | add() with an already active key. |
| `..._ERR_ITER_END` | Iterator at the logical end in the requested direction. |
| `..._ERR_ITER_INVALIDATED` | A successful mutation invalidated the cursor. |
| `..._ERR_SCHEMA_MISMATCH` | Stored schema id/version/record size differs from the expectation. |
| `..._ERR_FORMAT` | Magic, format version or geometry not understood. |
| `..._ERR_CORRUPT` | Integrity failure; recovery cannot determine a valid complete state. |
| `..._ERR_READONLY` | Mutation through a read-only handle. |
| `..._ERR_IO` | Backend I/O failure (short write, fsync, rename). |
| `..._ERR_RECOVERY_REQUIRED` | Read-only open found a state that needs writing to recover. |
| `..._ERR_IO_FAULTED` | Handle quarantined after an ambiguous I/O failure; close and reopen. |
| `..._ERR_STORAGE_FULL` | Candidate file could not be created/filled (volume full). |
| `..._ERR_ALREADY_EXISTS` | create() refused to overwrite an existing file. |
| `..._ERR_BUSY` | Another writable handle holds the lock file. |

Memory ownership: all handles are freed by `ringFileClose()` (always safe
to call, including on a failed operation's handle if you created it
yourself — the library never returns a partially usable handle through an
error). Record and key buffers are always caller-owned; the component never
retains a pointer to them. `ringFileValidate()` and the read operations
write nothing.

### Buffer contract

* Record buffers are caller-owned and at least `recordSize` bytes; the
  component reads/writes exactly `recordSize` bytes.
* Initialize all padding (e.g. `memset` the whole struct) before writing.
  Unchanged-update detection compares all `recordSize` bytes, including
  padding and the payload.
* Key arguments are caller-owned buffers of at least the descriptor's
  `keySize` bytes. Integer keys carry the raw little-endian field bytes of
  exactly the type width. Byte-array keys compare by exact byte equality.
  String keys are fixed-size char arrays, compared case-sensitively with
  bytes after the first terminator ignored; an unterminated key buffer is
  rejected with `ESP_ERR_INVALID_ARG`.
* Records must be pointer-free, fixed-width-field structs. Payloads are
  stored as raw bytes: reopening requires the same payload layout, ABI and
  little-endian byte order. Any layout change requires a new
  `schemaVersion`. Integers are extracted with `memcpy`, so records need
  no alignment beyond their own fields.

### Key types

`RING_FILE_KEY_S8/U8/S16/U16/S32/U32/S64/U64` (keySize must equal the type
width), `RING_FILE_KEY_BYTES` (fixed-size byte array, exact equality),
`RING_FILE_KEY_STRING` (bounded null-terminated, case-sensitive).

## Binary format

```
offset 0        [ 64-byte header ]
offset 64       [ slot 0 ][ slot 1 ] ... [ slot capacity ]   (capacity + 1 slots)
```

All metadata is explicitly little-endian encoded; compiler struct padding
never reaches storage.

Header (written only by create, resize and clear):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u32 | magic `RFS1` (0x52465331) |
| 4 | u16 | format version (1) |
| 6 | u16 | header size (64) |
| 8 | u64 | generation (incremented per resize) |
| 16 | u32 | schema id |
| 20 | u32 | schema version |
| 24 | u32 | record size |
| 28 | u32 | capacity |
| 32 | u32 | key offset |
| 36 | u32 | key type |
| 40 | u32 | key size |
| 44 | u32 | flags (0) |
| 48 | u64 | epoch (records with seq < epoch are hidden; set by `ringFileClear`) |
| 56 | u32 | CRC32 over bytes [0,56) |
| 60 | u32 | reserved (0xFFFFFFFF) |

Slot header (20 bytes) followed by `recordSize` raw record bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u32 | slot magic `RFS2` (0x52465332) |
| 4 | u64 | seq (insertion sequence, globally unique) |
| 12 | u32 | rev (revision, incremented per update; insertion order stays with seq) |
| 16 | u32 | CRC32 over the seq, rev and record bytes |

CRC is CRC-32/ISO-HDLC (reflected `0xEDB88320`, init `0xFFFFFFFF`, final
xor).

### Committed state derivation and commit protocol

The file has **capacity + 1** slots. A record is *committed* when its slot
carries the highest (seq, rev) among the valid copies of its key and fewer
than `capacity` committed members have a higher seq. Every mutation writes
exactly one slot image (header + record in a single `write()`):

* **add**: new (seq, rev 0) into a slot that is not currently committed.
  The evicted oldest member is never touched: it drops out of the derived
  set automatically and its slot becomes reusable scratch.
* **update**: same seq, rev + 1, into an uncommitted slot. The old copy
  remains readable until the new copy is on storage and wins on (seq, rev).
* **clear**: single header write advancing the epoch.

Because a committed slot is never overwritten in place, an interrupted
write can only ever produce a slot that fails its own CRC. After any
interruption the file therefore exposes a **complete previous or new
committed state** — never a mix. Redundant old copies of edited keys also
give a second candidate to the reopen logic. The extra (capacity + 1)th
slot is the invariant guarantor of a free scratch slot.

Header block (create/resize/clear) is a single 64-byte write; on LittleFS
this lands inside one COW block commit, on FATFS it is not atomic —
a damaged header is reported as `..._ERR_CORRUPT` (a checksum detects
corruption but cannot reconstruct; see durability).

### Resize

`ringFileResize` flushes staged data, then:

1. Creates `<path>.rfsnew` on the same volume, writes the header
   (generation + 1, new capacity) and streams the retained records
   (insertion order; on shrink the newest `newCapacity` are retained).
2. `fsync`s and verifies the candidate (header decode + slot scan).
3. `rename(path → path.rfsold)`, then `rename(path.rfsnew → path)`, then
   `unlink(path.rfsold)`.

Interruption handling at reopen: main file valid → use it and delete
leftovers; main file missing/damaged and `path.rfsold` valid → restore it
(read-only opens return `..._ERR_RECOVERY_REQUIRED` instead of writing).
A second rename failure is rolled back immediately; if the rollback also
fails the handle quarantines and the next writable open recovers the
complete previous state. Temporary storage: about
`64 + (retained + 1) × slotSize` bytes free on the volume. If the volume
is full the original is untouched and `..._ERR_STORAGE_FULL` (or
`..._ERR_IO`) is returned.

### Recovery and ambiguous I/O

Short reads, short writes, `fsync`, `close` and both renames are checked.
After an ambiguous failure (write or sync error) the handle quarantines:
every further mutating call returns `..._ERR_IO_FAULTED` until close and
reopen. Reads remain available. Reopen runs the recovery rules above.

## Durability by backend

* **LittleFS** (tested partition type `littlefs`, wear leveling enabled):
  power-cut safe at block level; a torn write surfaces as a CRC failure on
  the affected slot, and the component derives a complete state. Component
  recovery works against a *readable* filesystem; a lost superblock or a
  failing flash device is outside component recovery and surfaces as
  `..._ERR_FORMAT`/`..._ERR_CORRUPT`/`..._ERR_IO`.
* **FATFS on SD**: `fsync` maps to `f_sync` (file + directory entry write);
  FAT metadata updates are not atomic. The rename pair is atomic per
  operation (`f_rename`), but the pair as a whole is why the `.rfsold`
  backup exists. Cached card writes that never commit after a power cut
  can lose or corrupt more than the component can detect; the checksums
  detect this (no silent bad state) but cannot reconstruct missing data —
  `..._ERR_CORRUPT` is reported and the ring must be recreated. SD-internal
  wear leveling is invisible to the host.
* Keep LittleFS free space healthy (the example partition is 256 KiB for
  a 4-record demo; production: leave generous free space — LittleFS
  rewrites whole blocks on grow, so file growth is not wear-free).

## Persistence modes

* **Immediate** (default): a successful mutation is committed and
  synchronized through the backend before the call returns.
* **Buffered** (`RING_FILE_PERSIST_BUFFERED`, `maxPendingRecords`
  1..64, default 8): mutations stage in a caller-bounded RAM overlay.
  Reads through the same handle see staged changes; duplicate checks,
  eviction and update-or-add semantics all apply to staged state.
  Repeated edits of the same active record coalesce into one staged image.
  The buffer commits (slot images + `fsync`) when the limit is reached, on
  `ringFileSync()` and on a successful `ringFileClose()`. Buffered success
  acknowledges staging only: a power cut may lose everything since the
  last commit, and the age of staged data is unbounded unless the
  application schedules periodic syncs. `ringFileResize` commits staged
  data first; `ringFileClear` drops it.

## Storage wear

* Ordinary mutations (add, update, upsert) write exactly **one slot image
  plus one fsync**, independent of capacity: no full-ring rewrite, no
  fixed header or fixed journal location touched per commit, no component
  transaction metadata beyond the 20-byte slot header. Commit records
  rotate through all capacity + 1 slots.
* Repeated edits of the same key alternate between slots (old copy is
  superseded, then reused as scratch), so a single-record ring with a hot
  key spreads writes over two slots. Host-test assertion: 30 repeated
  edits = 30 writes + 30 syncs; 40 adds on a 4-slot ring = 40 + 40.
* Unchanged updates (identical key per the key rules and identical
  initialized payload bytes) produce **zero** writes and syncs. Reads,
  validation and clean close produce zero component-level writes.
* Buffered mode coalesces repeated edits of one record: N staged updates
  cost at most one committed image at the next flush.
* On LittleFS, `fsync` writes file metadata; LittleFS copy-on-write makes
  each file block rewrite a new physical program. File *growth* copies
  file blocks (realloc), so create-time growth of a lazily extended ring
  file costs LittleFS block programs. Pre-create the ring at full logical
  size (add-and-clear records, or platform-level preallocation) if the
  one-time grow cost matters. LittleFS block program/erase counters are
  not exposed through this component; the numbers above are **logical VFS
  I/O** and do not infer media lifetime.

## Complexity and resource estimates

With `n = capacity + 1` slot positions, bounded RAM independent of capacity:

| Operation | Storage scans (worst case) |
| --- | --- |
| add (with duplicate check), update, keyed read | O(n) slot reads per membership test; O(n²) slot-header reads overall |
| oldest/newest | O(n²) |
| iterator step | O(n²) |
| open (count/nextSeq derivation) | O(n²) |
| validate | O(n²) |
| resize | O(n²) plus streaming candidate |

The scan-one-slot-at-a-time design keeps RAM independent of capacity at
the cost of CPU for large rings; typical logger depths (tens to a few
hundred records) are fine.

Per handle: a few hundred bytes plus `slotSize + 2 × keySize` scratch and,
in buffered mode, `maxPendingRecords × (recordSize + 20)` bytes. A 24-slot
ring of 64-byte records with 8 buffered entries ≈ 1.2 KB RAM.

## Filesystem, locking and task context

The application mounts/unmounts the filesystem and chooses the path.
Operations run in task context and may block on storage I/O. Operations on
one writable handle are serialized by a FreeRTOS mutex. Across handles and
tasks a writable open takes an exclusive lock file
(`<path>.lck`, `O_EXCL`). A handle that never reached `ringFileClose()`
(panic, reset, power loss) cannot release the lock, so every later writable
open fails with `RING_FILE_STORE_ERR_BUSY` until it is removed: call
`ringFileBreakStaleLock()` right after a reset, before the first open, when
no live handle can exist (the example does this at startup). Read-only
opens neither lock nor write. Callers must close
every handle for a filesystem before unmounting it.

Because every call descends through the VFS and the storage layer, the
calling task needs headroom in its stack. The example's full
demonstration on LittleFS exceeded the 3584-byte ESP-IDF default main-task
stack; the overflow showed up as far-removed symptoms (random crashes in
`free()`/VFS and a stall in the flash IPC handshake) rather than a clean
stack-overflow message. Give tasks that use this component at least 8 KB
of stack (the example sets `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` in
`sdkconfig.defaults.esp32s3`).

## Build and test

```sh
# Host-side automated tests (no hardware needed; see test/host):
components/ring_file_store/test/run_tests.sh

# Example application (root project, ESP32-S3, ESP-IDF 6.0.2):
idf.py set-target esp32s3
idf.py build
```

The host suite injects failures at every write, sync and rename boundary
and verifies the reopened state; see `test/README.md`.

## ESP Component Registry

This directory is the package root (`idf_component.yml`). The core package
is independent of any mount point or SD pins — those live in the example
application. See the repository's `PUBLISHING.md` for release steps.
