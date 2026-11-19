<!--
 FileVersion: [<nr>] <date/time>


FileVersion: [2] 19-11-2026 - 11:40

ringFileStore - component README: usage, binary format, commit protocol.
Copyright (C) 2026 Willem Aandewiel (mrWheel)
SPDX-License-Identifier: GPL-3.0-or-later
-->

# ring_file_store

Persistent fixed-capacity ring files for ESP-IDF (tested against ESP-IDF
6.0.2, ESP32-S3). Records are caller-defined payload structs stored in
fixed-size binary slots on any ESP-IDF VFS filesystem (LittleFS, FATFS on
SD). **Every file is timestamp-keyed**: the component always prepends two
automatic key fields — the full `timeStamp` and the derived `usedKey` — to
the stored record, and returns them on every read. The caller never
declares or describes a key field. "Oldest" and "newest" always mean
bucket (period) order; the DSMRlogger32 pattern.

The design reuses the fixed-slot storage and modulo slot addressing of
[DSMRlogger32](https://github.com/mrWheel/DSMRlogger32) (direct offset seek
into fixed-width history slots) and adds a versioned binary format and a
crash-recoverable commit protocol. Legacy DSMR CSV import is not part of
this component.

```c
#include "ring_file_store.h"

//-- Payload struct: NO key fields, the component adds them.
typedef struct
{
  uint32_t energyWh;
  uint16_t errorCount;
  uint16_t pad;
} meterRecord_t;

typedef struct
{
  ringFileKeyFields_t keys;   //-- timeStamp + usedKey, added by the component
  meterRecord_t meter;
} fullMeter_t;                //-- read buffer type (16 + 8 bytes)

ringFileConfig_t cfg = {0};
cfg.maxRecords = 24;                            //-- 24 hourly buckets
cfg.schemaId = RING_FILE_SCHEMA_YYMMDDHH;       //-- "YYMMDDhh"
cfg.recordSize = sizeof(meterRecord_t);         //-- payload only

ringFileHandle_t *ring = NULL;
ESP_ERROR_CHECK(ringFileCreate("/rfs/meter.rfs", &cfg, &ring));

meterRecord_t rec = {0};
rec.energyWh = 5000;
ESP_ERROR_CHECK(ringFileAdd(ring, 20261023140000ULL, &rec));  //-- hour 14

fullMeter_t out;
ESP_ERROR_CHECK(ringFileReadByKey(ring, 2026102314ULL, &out));
//-- out.keys.timeStamp == 20261023140000, out.keys.usedKey == 2026102314
ringFileClose(ring);
```

## Timestamp key model

Every file is created with a **file type** (`schemaId`): one of the fixed
strings `"YY"`, `"YYMM"`, `"YYMMDD"`, `"YYMMDDhh"`, `"YYMMDDhhmm"` or
`"YYMMDDhhmmss"` (case-sensitive, lower-case `hh`/`mm`/`ss`, the
two-digit-year DSMRlogger32 form). Timestamps are numeric `uint64_t`
values in `EEYYMMDDhhmmss` form (14 digits, century fixed `20`),
back-filled with `00` values below the file type. The stored record is
always:

```
[ u64 timeStamp ][ u64 usedKey ][ caller payload bytes ]
   field 1          field 2        your struct only
```

* `timeStamp` (field 1) — the complete timestamp as supplied (digits below
  the file type may be zero padding or real values, e.g.
  `20261118000000` or `20261118000001` for an `"YYMMDD"` file).
* `usedKey` (field 2) — the timestamp truncated to the file-type digits:
  type `"YYMMDDhh"`, timestamp `20261118143055` → `usedKey 2026111814`
  (the DSMRlogger32 `YYMMDDHH` column, century included). All key
  comparisons use `usedKey`.
* **bucketId** — `period % maxRecords`, the DSMRlogger-style logical slot
  reported by `ringFileIteratorGetSlot()`:

  | `schemaId` | period ("berekende waarde") | boundary |
  | --- | --- | --- |
  | `"YY"` | year | year start |
  | `"YYMM"` | `(year - 1) * 12 + month` | month start (DSMRlogger32; 2026-10 → 24310) |
  | `"YYMMDD"` | days since 1970-01-01 | midnight |
  | `"YYMMDDhh"` | days * 24 + hour | hour |
  | `"YYMMDDhhmm"` | days * 1440 + hour * 60 + minute | minute |
  | `"YYMMDDhhmmss"` | days * 86400 + hour * 3600 + minute * 60 + second | second |

Oldest/newest order, iteration order, eviction and resize retention all
follow the **period**. Physical placement stays with the crash-safe commit
protocol below (a committed slot is never overwritten in place), so the
reported bucketId is the logical DSMR-style position, not the on-storage
slot index.

Digit validation per granularity: century `20`; month 01–12; day 01–31
valid for the month (leap years included); hour 00–23; minute/second
00–59. Fields **below** the file type accept zero padding or a valid
value; anything else is `ESP_ERR_INVALID_ARG`.

### Time-ordered writes

* **ringFileAdd**: if the `usedKey` is already active its record is
  overwritten (same bucket = same record); otherwise the record is
  appended when the bucket is **newer than the newest stored bucket**.
  A bucket that is not newer (clock rewind, or filling a gap) is rejected
  with `..._ERR_NOT_NEWER` without any change — use `ringFileEdit`.
* **ringFileUpsert**: updates the active record of the bucket for **any**
  timestamp within the bucket — the last-added record therefore stays
  updatable while time stays in its bucket (the hourly-logger pattern);
  otherwise the add rules apply.
* **ringFileEdit**: backfill/edit for lost measurements. Updates the
  bucket when active (any timestamp); otherwise inserts the bucket when
  its period lies inside the retained window
  `[newestPeriod - capacity + 1, newestPeriod]` (an empty ring accepts
  any bucket). Outside that window the bucket was evicted long ago:
  `..._ERR_BUCKET_OUT_OF_RANGE`, nothing changes.

`ringFileReadByKey` / `ringFileUpdateByKey` / iterator-by-key address
records by the plain `uint64_t` `usedKey` value. `ringFileClear`, resize,
locking, buffered mode and the commit protocol are mode-independent.

## API

| Function | Purpose |
| --- | --- |
| `ringFileCreate` | Create an empty ring exclusively and return it open read-write. Never recreates or truncates (`RING_FILE_STORE_ERR_ALREADY_EXISTS`). |
| `ringFileOpen` | Open existing, read-only or read-write. Validates the expected `schemaId` string and payload record size. A read-write open may perform recovery writes; a read-only open never writes and returns `RING_FILE_STORE_ERR_RECOVERY_REQUIRED` if recovery is needed. |
| `ringFileClose` | Commit staged data, close, free. A clean handle performs no write and no sync. |
| `ringFileAdd` | Append `(timeStamp, payload)` as newest; the component stores the two key fields and the bucketId. Same bucket active → update in place; bucket not newer than the newest → `..._ERR_NOT_NEWER`; at capacity exactly the oldest bucket is evicted. |
| `ringFileReadByKey` | Copy the full stored record (timestamp, usedKey, payload) for a `usedKey`. |
| `ringFileUpdateByKey` | Replace the complete record of an active `usedKey`, preserving its bucket position. Timestamp deriving a different `usedKey`: `ESP_ERR_INVALID_ARG`. Identical replacement: no write at all. |
| `ringFileUpsert` | Atomic update-or-add under the handle lock (the hourly-logger pattern). |
| `ringFileEdit` | Backfill/edit a bucket (lost measurements). Updates the active record of the bucket, otherwise inserts it when its period lies inside the retained window. |
| `ringFileReadOldest` / `ringFileReadNewest` | First/last record in bucket order (full record, key fields included). |
| `ringFileIteratorBegin/Next/Previous` | Caller-owned cursors; start at oldest, newest or an exact `usedKey`; traverse either direction; `ITER_END` at the logical ends; successful mutations invalidate cursors. |
| `ringFileIteratorGetSlot` | bucketId (`period % capacity`) of the record the cursor last returned — the DSMRlogger-style logical slot (`RING_FILE_SLOT_STAGED` for an unflushed staged record). For diagnostics and test logging. |
| `ringFileResize` | Change capacity through a verified candidate file plus an atomic rename switch. Shrink keeps the newest buckets that fit. Capacity ≥ 1. Persists the new capacity. |
| `ringFileGetInfo` | count, capacity, payload size, stored record size, `schemaId`, granularity, storage size, pending count, dirty. |
| `ringFileSync` | Commit pending buffered data and synchronize through the backend. |
| `ringFileValidate` | Stand-alone read-only integrity check; distinguishes bad metadata from bad record slots and reports stray resize files. |
| `ringFileBreakStaleLock` | Remove the `<path>.lck` lock file left by an interrupted run. Only for use right after a reset, before the first open, when no live handle can exist. |
| `ringFileClear` | Discard all records (epoch bump in the header). File type and capacity retained. |
| `ringFileGranularityFromSchemaId` | Map a `schemaId` string to its `ringFileGranularity_t`. |
| `ringFileTimeStampToUsedKey` / `ringFileTimeStampToPeriod` | Compute the `usedKey` / period ("berekende waarde") of a timestamp for a granularity (validates the digits). |
| `ringFileTimeStampStringToUnix` / `ringFileUnixToTimeStampString` | Convert between unix seconds and the 14-digit `"EEYYMMDDHHMMSS"` string (century fixed `20`, years 2000–2099). |
| `ringFileUnixToBucketId` | Period for a unix timestamp at a granularity (the bucket source; `period % capacity` is the bucketId). |
| `ringFileUnixToIsoWeek` | ISO-8601 week-numbering year and week number (weeks start Monday) for display. |

### Result codes

Standard: `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`,
`ESP_ERR_INVALID_STATE`, `ESP_ERR_NOT_FOUND`. Custom, base
`RING_FILE_STORE_ERR_BASE` (0x5600):

| Code | Meaning |
| --- | --- |
| `..._ERR_RING_EMPTY` | readOldest/readNewest/iterator on a ring with no records. |
| `..._ERR_KEY_NOT_FOUND` | No active record carries the `usedKey`. |
| `..._ERR_DUPLICATE_KEY` | Reserved — no longer produced (adding an active bucket updates that record). |
| `..._ERR_ITER_END` | Iterator at the logical end in the requested direction. |
| `..._ERR_ITER_INVALIDATED` | A successful mutation invalidated the cursor. |
| `..._ERR_SCHEMA_MISMATCH` | Stored `schemaId` string or payload record size differs from the expectation. |
| `..._ERR_FORMAT` | Magic, format version or geometry not understood. |
| `..._ERR_CORRUPT` | Integrity failure; recovery cannot determine a valid complete state. |
| `..._ERR_READONLY` | Mutation through a read-only handle. |
| `..._ERR_IO` | Backend I/O failure (short write, fsync, rename). |
| `..._ERR_RECOVERY_REQUIRED` | Read-only open found a state that needs writing to recover. |
| `..._ERR_IO_FAULTED` | Handle quarantined after an ambiguous I/O failure; close and reopen. |
| `..._ERR_STORAGE_FULL` | Candidate file could not be created/filled (volume full). |
| `..._ERR_ALREADY_EXISTS` | create() refused to overwrite an existing file. |
| `..._ERR_BUSY` | Another writable handle holds the lock file. |
| `..._ERR_NOT_NEWER` | `ringFileAdd`/`ringFileUpsert` add path for a bucket not newer than the newest stored bucket (clock rewind or gap fill — use `ringFileEdit`). |
| `..._ERR_BUCKET_OUT_OF_RANGE` | `ringFileEdit` for a bucket outside the retained window `[newestPeriod - capacity + 1, newestPeriod]`. |

Memory ownership: all handles are freed by `ringFileClose()` (always safe
to call, including on a failed operation's handle if you created it
yourself — the library never returns a partially usable handle through an
error). Record and output buffers are always caller-owned; the component
never retains a pointer to them. `ringFileValidate()` and the read
operations write nothing.

### Buffer contract

* **Payload** buffers (`ringFileAdd`, `ringFileUpsert`, `ringFileEdit`,
  `ringFileUpdateByKey`) are caller-owned and at least `recordSize`
  bytes — the payload struct only; the component adds the two key fields.
* **Output record** buffers (all reads and iterator steps) must hold
  `RING_FILE_FULL_RECORD_SIZE(payloadType)` bytes: the
  `ringFileKeyFields_t` prefix (`timeStamp`, then `usedKey`) followed by
  the payload. The component fills all of it.
* Initialize all payload padding (e.g. `memset` the whole struct) before
  writing. Unchanged-update detection compares the full stored record
  (key fields and padding included).
* Timestamp arguments are numeric `EEYYMMDDhhmmss` `uint64_t` values;
  key arguments are plain `uint64_t usedKey` values.
* Payloads must be pointer-free, fixed-width-field structs. They are
  stored as raw bytes: reopening requires the same payload layout, ABI
  and little-endian byte order. Any payload layout change requires
  deleting the old file and creating a new one. Integers are stored
  little-endian and read with `memcpy`, so no unaligned loads are issued.

## Binary format

```
offset 0        [ 96-byte header ]
offset 96       [ slot 0 ][ slot 1 ] ... [ slot capacity ]   (capacity + 1 slots)
```

All metadata is explicitly little-endian encoded; compiler struct padding
never reaches storage.

Header (written only by create, resize and clear; format version 4):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u32 | magic `RFS1` (0x52465331) |
| 4 | u16 | format version (4) |
| 6 | u16 | header size (96) |
| 8 | u64 | generation (incremented per resize) |
| 16 | 16 B | `schemaId` string, NUL-terminated, max 12 chars; unused bytes 0xFF |
| 32 | u32 | stored record size (16 + payload size) |
| 36 | u32 | capacity |
| 40 | u32 | granularity (1 year .. 6 second) |
| 44 | u32 | flags (0) |
| 48 | u64 | epoch (records with seq < epoch are hidden; set by `ringFileClear`) |
| 56 | 32 B | reserved (0xFFFFFFFF) |
| 88 | u32 | CRC32 over bytes [0,88) |

Slot header (20 bytes) followed by the stored record:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u32 | slot magic `RFS2` (0x52465332) |
| 4 | u64 | seq (write sequence, globally unique; placement/commit order) |
| 12 | u32 | rev (revision, incremented per update of the same bucket) |
| 16 | u32 | CRC32 over the seq, rev and the stored record bytes |
| 20 | u64 | `timeStamp` — full EEYYMMDDhhmmss (automatic field 1) |
| 28 | u64 | `usedKey` — truncated timestamp (automatic field 2, the key) |
| 36 | … | caller payload bytes |

CRC is CRC-32/ISO-HDLC (reflected `0xEDB88320`, init `0xFFFFFFFF`, final
xor).

### Committed state derivation and commit protocol

The file has **capacity + 1** slots. A record is *committed* when its slot
carries the highest (seq, rev) among the valid copies of its `usedKey` and
fewer than `capacity` committed members have a higher period (bucket
order). Every mutation writes exactly one slot image (header + record in a
single `write()`):

* **add**: new (seq, rev 0) into a slot that is not currently committed.
  The evicted oldest bucket is never touched: it drops out of the derived
  set automatically and its slot becomes reusable scratch.
* **update**: same seq, rev + 1, into an uncommitted slot. The old copy
  remains readable until the new copy is on storage and wins on (seq, rev).
* **clear**: single header write advancing the epoch.

Because a committed slot is never overwritten in place, an interrupted
write can only ever produce a slot that fails its own CRC. After any
interruption the file therefore exposes a **complete previous or new
committed state** — never a mix. Redundant old copies of edited buckets
also give a second candidate to the reopen logic. The extra (capacity + 1)th
slot is the invariant guarantor of a free scratch slot.

Header block (create/resize/clear) is a single 96-byte write; on LittleFS
this lands inside one COW block commit, on FATFS it is not atomic —
a damaged header is reported as `..._ERR_CORRUPT` (a checksum detects
corruption but cannot reconstruct; see durability).

### Resize

`ringFileResize` flushes staged data, then:

1. Creates `<path>.rfsnew` on the same volume, writes the header
   (generation + 1, new capacity) and streams the retained records
   (bucket order; on shrink the newest `newCapacity` buckets are
   retained).
2. `fsync`s and verifies the candidate (header decode + slot scan).
3. `rename(path → path.rfsold)`, then `rename(path.rfsnew → path)`, then
   `unlink(path.rfsold)`.

Interruption handling at reopen: main file valid → use it and delete
leftovers; main file missing/damaged and `path.rfsold` valid → restore it
(read-only opens return `..._ERR_RECOVERY_REQUIRED` instead of writing).
A second rename failure is rolled back immediately; if the rollback also
fails the handle quarantines and the next writable open recovers the
complete previous state. Temporary storage: about
`96 + (retained + 1) × slotSize` bytes free on the volume. If the volume
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
  a small demo; production: leave generous free space — LittleFS
  rewrites whole blocks on grow, so file growth is not wear-free).

## Persistence modes

* **Immediate** (default): a successful mutation is committed and
  synchronized through the backend before the call returns.
* **Buffered** (`RING_FILE_PERSIST_BUFFERED`, `maxPendingRecords`
  1..64, default 8): mutations stage in a caller-bounded RAM overlay.
  Reads through the same handle see staged changes; duplicate-bucket
  checks, eviction and update-or-add semantics all apply to staged state.
  Repeated edits of the same active bucket coalesce into one staged image.
  The buffer commits (slot images + `fsync`) when the limit is reached, on
  `ringFileSync()` and on a successful `ringFileClose()`. Buffered success
  acknowledges staging only: a power cut may lose everything since the
  last commit, and the age of staged data is unbounded unless the
  application schedules periodic syncs. `ringFileResize` commits staged
  data first; `ringFileClear` drops it.

## Storage wear

* Ordinary mutations (add, update, upsert, edit) write exactly **one slot
  image plus one fsync**, independent of capacity: no full-ring rewrite,
  no fixed header or fixed journal location touched per commit, no
  component transaction metadata beyond the 20-byte slot header. Commit
  records rotate through all capacity + 1 slots.
* Repeated edits of the same bucket alternate between slots (old copy is
  superseded, then reused as scratch), so a single-record ring with a hot
  bucket spreads writes over two slots. Host-test assertion: 30 repeated
  edits = 30 writes + 30 syncs; 40 adds on a 4-slot ring = 40 + 40.
* Unchanged updates (same bucket and identical initialized payload bytes,
  stored key fields included) produce **zero** writes and syncs. Reads,
  validation and clean close produce zero component-level writes.
* Buffered mode coalesces repeated edits of one bucket: N staged updates
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
| add (with bucket check), update, keyed read | O(n) slot reads per membership test; O(n²) slot-header reads overall |
| oldest/newest | O(n²) |
| iterator step | O(n²) |
| open (count/nextSeq derivation) | O(n²) |
| validate | O(n²) |
| resize | O(n²) plus streaming candidate |

The scan-one-slot-at-a-time design keeps RAM independent of capacity at
the cost of CPU for large rings; typical logger depths (tens to a few
hundred records) are fine.

Per handle: a few hundred bytes plus `slotSize + 2 × 8` scratch, one
`storedRecordSize` build buffer and, in buffered mode,
`maxPendingRecords × slotSize` bytes. A 24-slot ring of 64-byte payloads
with 8 buffered entries ≈ 1.4 KB RAM.

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
and verifies the reopened state; see `test/README.md`. It also writes
`testLog.md` (repository root) with every logged command, its result, and
ring dumps showing `usedKey`, `timeStamp` and bucketId per record.

## ESP Component Registry

This directory is the package root (`idf_component.yml`). The core package
is independent of any mount point or SD pins — those live in the example
application. See the repository's `PUBLISHING.md` for release steps.
