<!--
 FileVersion: [4] 19-11-2026 - 15:25

 ringFileStore - project prompt: requirements and rules for this project.
 Copyright (C) 2026 Willem Aandewiel (mrWheel)
 SPDX-License-Identifier: GPL-3.0-or-later
-->

# Implement a persistent ring-file component for ESP-IDF

Implement a reusable ESP-IDF component named `ring_file_store` in readable C for ESP-IDF 6.0.2, with ESP32-S3 as the initial target. Use Allman braces, lowerCamelCase for local variables and English comments. Minimizing storage wear is a primary design requirement, subject to the selected durability policy. Use an opaque handle and a public C header with an `extern "C"` guard for C++ callers.

## Reference implementation

The design builds on the fixed-width CSV history pattern used for hour, day and month history. Its first column is the timestamp truncated to the file type (for an hourly file `YYMMDDHH`); the implementation calculates the slot from the period number modulo the configured history depth, then seeks directly to the corresponding file offset. Reuse the fixed-slot storage principle and the modulo slot addressing. The new component owns the timestamp key handling itself (see "Timestamp keys and file types") and uses its own versioned binary format. Legacy CSV import is outside this implementation.

## Timestamp keys and file types

- Every ring file created by this component is timestamp-keyed. The key is never
  described by the caller: the component always prepends two fields to the
  stored record. Stored record layout:
  `[ u64 fullTimeStamp ][ u64 usedKey ][ caller payload bytes ]`.
  The caller's record struct contains only the payload; the caller never
  declares or fills the two key fields. Every read operation returns the full
  record, so both automatically added fields are delivered back to the caller
  (field 1: the complete timestamp, field 2: the `usedKey`, named after the
  first-column key such as `YYMMDDHH`).
- At creation the caller supplies the VFS path, the file type as `schemaId`,
  the maximum record count and the payload record size. The `schemaId` is one
  of the fixed strings `"YY"`, `"YYMM"`, `"YYMMDD"`, `"YYMMDDhh"`,
  `"YYMMDDhhmm"` or `"YYMMDDhhmmss"` (case-sensitive, lower-case `hh`, `mm`,
  `ss`). The two-digit year is the compact form; the component stores and
  receives timestamps in the century-padded `EEYY…` form (century fixed to
  "20", years 2000–2099).
- Timestamps are passed and returned as numeric `uint64_t` values in
  `EEYYMMDDhhmmss` form (14 digits), back-filled with `00` values below the
  file type: an `EEYYMMDD`-type record for 18-11-2026 is stored as
  `20261118000000`. Digits below the file-type granularity are validated
  normally (hour 00–23, minute/second 00–59) but never take part in the key;
  a caller may keep real sub-granularity digits.
- `usedKey` (stored field 2) is the timestamp truncated to the file-type
  digits: for an `YYMMDDhh` file the timestamp `20261118143055` has
  `usedKey 2026111814`. Key equality, duplicate-bucket detection, keyed
  reads, keyed iteration and update key matching all compare `usedKey`.
- Depending on the file type a period value is calculated from the
  timestamp; `<periodValue> % <maxRecords>`
  determines the bucketId, the logical slot reported by the
  iterator. The period formulas are:
  | schemaId | period |
  | --- | --- |
  | `"YY"` | year |
  | `"YYMM"` | `(year - 1) * 12 + month` (month formula) |
  | `"YYMMDD"` | days since 1970-01-01 |
  | `"YYMMDDhh"` | days since 1970-01-01 * 24 + hour |
  | `"YYMMDDhhmm"` | days since 1970-01-01 * 1440 + hour * 60 + minute |
  | `"YYMMDDhhmmss"` | days since 1970-01-01 * 86400 + hour * 3600 + minute * 60 + second |
  Order, oldest/newest, eviction and resize retention follow the period
  value; the physical crash-safe slot placement stays with the commit
  protocol (the reported bucketId is the logical position).
- Use caller-owned structs for reads and writes. Payloads are stored as raw
  bytes; document that reopening requires the same payload layout, ABI and
  endianness. Use pointer-free payload structs with fixed-width fields,
  initialize padding before writing, and require the application to create a
  new file (delete the old one) when the payload layout changes. Encode file
  metadata explicitly, independent of compiler struct padding.
- Persist the file type (`schemaId` string), granularity and payload size in
  the file. Validate the expected file type and payload size on open. Use
  capacity from the file on reopen and change it only through an explicit
  resize. Validate bounds, file length and arithmetic before allocating memory
  or seeking. Support multiple independent ring files with different types.

## Required API and semantics

Use `esp_err_t` and caller-owned output buffers with explicit sizes. Define and document distinct results for an empty ring, key not found, end of iteration, invalidated iterator, invalid arguments, incompatible file type or format, corruption, read-only access and I/O failures. Document memory ownership and error behavior.

| Operation | Required behavior |
| --- | --- |
| `ringFileCreate`, `ringFileOpen`, `ringFileClose` | Create an empty file exclusively. Open an existing file in read-only or read-write mode. Report failures without silently recreating or truncating the data. |
| `ringFileAdd` | Takes the timestamp and the payload record. The component validates the timestamp digits, stores the full timestamp as field 1 and the derived `usedKey` as field 2, and computes the bucketId. Adding the `usedKey` of an active record updates that record in place; otherwise append as newest and, at capacity, evict exactly the oldest record. Reject a timestamp whose bucket is older than the newest stored bucket (clock rewind or gap fill) without changing any records; `ringFileEdit` covers backfill. |
| `ringFileReadByKey` | Copy the full stored record (timestamp, usedKey, payload) of the matching `usedKey` into the caller's output buffer. |
| `ringFileUpdateByKey` | Takes the lookup `usedKey`, the new timestamp and the payload. Replace the complete record for an existing `usedKey` while preserving its bucket position. Reject a replacement whose timestamp derives a different `usedKey` than the lookup key. |
| `ringFileReadOldest`, `ringFileReadNewest` | Copy the first or last record in bucket order (timestamp, usedKey, payload) into the caller's output buffer. |
| `ringFileIteratorBegin`, `ringFileIteratorNext`, `ringFileIteratorPrevious` | Start at the oldest or newest bucket or at an exact `usedKey`. Traverse in either direction and copy each full record into a caller-owned buffer. Stop at the logical ends. Give each caller its own cursor. Report the bucketId (`period % capacity`) per returned record. A successful mutation invalidates existing cursors on that handle. |
| `ringFileResize` | Increase or decrease capacity while preserving the relative order of retained records. On shrink, retain the newest buckets that fit. Support capacity one and reject zero. Persist the new capacity. |
| `ringFileGetInfo` | Report count, capacity, payload size, stored record size, `schemaId` string, granularity and storage size. |
| `ringFileSync` | Commit pending buffered changes and synchronize through the backend. Clean handles perform no write or sync. Report failures and document backend guarantees. |
| `ringFileValidate` | Provide a read-only integrity check with a result that identifies bad metadata or records. |
| `ringFileClear` | Explicitly discard all records while retaining the file type and capacity. Apply the same commit and recovery rules as for other mutations. |

"Oldest" and "newest" refer to bucket (period) order. Adding records
out of chronological order is only allowed through `ringFileEdit` within the
retained window `[newestPeriod - capacity + 1, newestPeriod]`. A full ring
holding the ten hourly buckets 08:00–17:00 must evict 08:00 when 18:00 is
added. Updating an active bucket preserves its position. Shrinking that ring
to two records retains 16:00 and 17:00.

Document a practical update-or-add pattern for a logger that revises the current hourly record. If a combined function is implemented, hold the lock throughout lookup and mutation and preserve the position of an existing record.

## Storage, consistency and resource limits

- Support mounted LittleFS and SD card storage using FATFS through ESP-IDF VFS. The application mounts and unmounts each filesystem. Verify the actual seek, sync and rename behavior for the targeted integrations. Provide mount configuration only in the examples.
- Use fixed-size binary slots. Keep ordinary mutations bounded to a small number of slot and metadata writes. Use bounded RAM independent of capacity, and stream resize operations. An initial implementation can scan keys one slot at a time. Document complexity separately for append with duplicate checking, keyed reads and edits, sequential iteration, validation and resize.
- Define a versioned file format with a magic value, checksummed metadata and per-record integrity checks. Persist enough information to identify the committed ring state and record order. Specify a recoverable commit protocol before coding. Metadata redundancy alone cannot restore the previous version of a slot overwritten by a partly completed add or edit. Use a spare slot, an undo journal or another documented mechanism that preserves the previous committed data until recovery can select a complete state. Check all short reads, short writes, flushes, syncs, closes and renames. After an ambiguous I/O failure, prevent further writes through that handle until reopened and recovered.
- Resize using a candidate on the same volume. Verify it before switching to it. Keep enough information to recover the previous or new complete state after interruption, including interruption between rename operations. Document temporary storage requirements and behavior when the volume is full. Preserve the committed ring on recoverable errors. Report corruption explicitly when recovery cannot determine a valid state.
- Describe the durability guarantees separately for LittleFS and FATFS. Distinguish component-level recovery on a readable filesystem from device-level failures such as SD removal, loss of filesystem metadata or a card failing to commit cached writes. A checksum detects corruption and cannot reconstruct missing data.
- Serialize operations on one writable handle with a FreeRTOS mutex. Specify exclusive file access across handles and tasks, and how the caller coordinates close and unmount. The API runs in task context and may block on storage I/O. Support read-only open without making repairs or modifying files. If recovery requires writes, return a clear recovery-required error.

## Storage wear and durability policy

- Minimize both write frequency and write amplification, including transaction metadata. Regular adds and edits must never rewrite the entire ring or rewrite a fixed header or fixed journal location on every commit. Distribute commit records across a bounded set of reusable locations or derive committed state from rotating slot metadata. Apply this to repeated edits of the same key, including a single-record ring. Preserve recoverability when those locations wrap. Document extra storage, cleanup frequency and write cost, including any filesystem-level copying observed on LittleFS.
- Skip writes for unchanged records. Use the defined key equality and initialized payload bytes when deciding whether a replacement is identical. Reads, validation and clean close must never generate component-level writes. Writable open may perform recovery when required.
- Offer two explicit persistence modes. In immediate mode, the default, a successful mutation is committed and synchronized through the backend. In opt-in buffered mode, stage changes in a bounded RAM buffer, coalesce repeated updates to the same active record where that preserves observable semantics, and commit when the configured buffer limit is reached, on explicit sync or on successful close. Reads on the handle see staged changes. Preserve insertion, eviction and key uniqueness semantics even before a flush. Define limits and the handling of resize and clear with pending data. Document the maximum number of pending mutations and that their age is unbounded unless the application schedules periodic syncs. Buffered success acknowledges a staged operation. A power cut may lose the unsynchronized changes. Preserve a recoverable committed state under either mode within the stated backend guarantees.
- Keep LittleFS wear leveling enabled and document the tested partition and free-space assumptions. On SD with FATFS, limit host writes and unnecessary filesystem syncs. Verify whether the selected FATFS settings add implicit syncs and account for them. Physical wear leveling on SD is managed by the card and may be unobservable.
- Show a logging example that accumulates the current hourly meter record in RAM and commits at the hourly boundary, with optional periodic syncs if interim recovery is required. Demonstrate how to stage equivalent updates through the buffered API and document the power-loss window for each example.
- Measure bytes written, write and sync counts, and the distribution of logical write offsets during repeated edits of one key and during multiple full ring rotations. Compare immediate mode with buffered updates for the same workloads, including the initial and final flush in the measurements. Where instrumentation is available, measure LittleFS block program and erase operations as well. Label VFS measurements as logical I/O and avoid inferring media lifetime from them. Test that unchanged updates produce no writes, that buffered edits coalesce, and that interrupting a flush leaves a recoverable state.

## Source file headers

Start each `<document>.md` and project-authored C source file (`.c`) and header file (`.h`),
including example and test files, with the following comment. Place it
before any `#include` directive or include guard. Replace the bracketed
text with a short description specific to that file.

```c
//--
//-- FileVersion: [<nr>] <date/time>
//--
//-- ringFileStore - <brief description of what this file does>
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
```

Use HTML-Comment block for all `<document>.md` files

```text
<!--
 FileVersion: [<nr>] <date/time>

 ringFileStore - <brief description of what this file does>
 Copyright (C) 2026 Willem Aandewiel (mrWheel)
 SPDX-License-Identifier: GPL-3.0-or-later
-->
```

`<nr>` must be incremented at every change of this file. `<date/time>` as "DD-MM-EEYY - HH:MM".

Use Europe/Amsterdam time

## Deliverables and verification

1. Supply a drop-in `components/ring_file_store/` directory with source, public header and `CMakeLists.txt`. Include dependency metadata wherever an example needs a managed component.
2. Provide a README with the API and error table, the timestamp key model
   (file types, stored record prefix, `usedKey` and bucketId formulas), binary
   format, transaction and recovery rules, storage and RAM estimates, and
   build instructions.
3. Provide an ESP32-S3 example that runs the same ring operations with LittleFS or an SD card. Use a sample meter payload record and numeric `EEYYMMDDhhmmss` timestamps, and demonstrate at least an hourly (`"YYMMDDhh"`) and a monthly (`"YYMM"`) file type. Demonstrate create, reopen, wraparound, lookup by `usedKey`, edit/backfill, iteration in both directions and resize.
4. Add automated tests for empty and single-slot rings, same-bucket overwrite, `usedKey` and bucketId calculation for every file type, out-of-order backfill inside and outside the retained window, updates, iterator invalidation, file type mismatch, reopen, corruption, resizing after wraparound and insufficient space. Inject failures around each mutation's write, sync and rename boundaries and check the resulting state after reopening. Check that a recoverable interruption exposes a complete previous or new state.
5. Build the examples with ESP-IDF 6.0.2. Report the commands and observed results, identifying which tests were executed with simulated failures and which storage backends were tested on hardware.
6. After the first successful call to `ringFileCreate()` or `ringFileOpen()`
   after boot, log the following notice using one `ESP_LOGI()` call per line
   with the tag `ringFileStore`. Log the notice at most once per boot,
   including when multiple files are opened or created.

   ```text
   ringFileStore  Copyright (C) 2026 Willem Aandewiel
   This program comes with ABSOLUTELY NO WARRANTY.
   This is free software, and you are welcome to redistribute it
   under the terms described in the LICENSE file.
   ```

## Coding and documentation rules
 * All comments, messages and documentation in English
 * Use Allman brace style.
 * Use two spaces for indentation.
 * Use lowerCamelCase for functions and variables, except in strict component code where PascalCase naming is used.
 * Write all code comments and documentation in English.
 * Prefix every code comment with //-- .
 * Never use /* ... */ comments.
 * Place comments above the code they describe, not beside them.

## Registry packaging

Maintain the component manifest and README in `components/ring_file_store/`.
Keep the core package independent of a particular filesystem mount or SD pin
assignment. Keep those dependencies and configuration in the example app.
Follow `PUBLISHING.md` when preparing a release. Publishing is a separate,
manual action for the maintainer.
