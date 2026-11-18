```
FileVersion: [1] 18-11-2026 - 10:11
```
# Implement a persistent ring-file component for ESP-IDF

Implement a reusable ESP-IDF component named `ring_file_store` in readable C for ESP-IDF 6.0.2, with ESP32-S3 as the initial target. Use Allman braces, lowerCamelCase for local variables and English comments. Minimizing storage wear is a primary design requirement, subject to the selected durability policy. Use an opaque handle and a public C header with an `extern "C"` guard for C++ callers.

## Reference implementation

Review [DSMRlogger32](https://github.com/mrWheel/DSMRlogger32/tree/main/DSMRlogger32), particularly [FSYSstuff.cpp](https://github.com/mrWheel/DSMRlogger32/blob/b9e401a090a2d0f937dcf817570d2a7970852386/DSMRlogger32/src/FSYSstuff.cpp) and [timeStuff.cpp](https://github.com/mrWheel/DSMRlogger32/blob/b9e401a090a2d0f937dcf817570d2a7970852386/DSMRlogger32/src/timeStuff.cpp). The logger stores hour, day and month history in fixed-width CSV slots. It calculates the slot from the period number modulo the configured history depth, then seeks directly to the corresponding file offset. Reuse the fixed-slot storage principle. The new component uses application-defined keys and insertion order, with its own versioned binary format. Legacy DSMR CSV import is outside this implementation.

## File schema and configuration

- At creation, the caller supplies the VFS path, maximum record count, schema identifier and version, C record size, and a descriptor for the key field inside that record. The descriptor defines its byte offset, type and size. Use `sizeof` and `offsetof` in the example.
- Support signed and unsigned fixed-width integers of 8, 16, 32 and 64 bits, fixed-size byte arrays, and bounded null-terminated strings stored in fixed-size character arrays. Compare keys by value. Byte arrays use exact byte equality. Strings are case-sensitive and bytes after the first terminator are ignored. Reject unterminated string keys. Access integer fields without unaligned loads. Document the buffer contract for key arguments.
- Use caller-owned structs for reads and writes. For the first release, store fixed-size payloads as raw bytes and document that reopening requires the same payload layout, ABI and endianness. Use pointer-free records with fixed-width fields, initialize padding before writing, and require an explicit schema version change when the layout changes. Encode file metadata explicitly, independent of compiler struct padding.
- Persist the schema and capacity in the file. Validate the expected schema on open. Use capacity from the file on reopen and change it only through an explicit resize. Validate bounds, file length and arithmetic before allocating memory or seeking. Support multiple independent ring files.

## Required API and semantics

Use `esp_err_t` and caller-owned output buffers with explicit sizes. Define and document distinct results for an empty ring, key not found, duplicate key, end of iteration, invalidated iterator, invalid arguments, incompatible schema or format, corruption, read-only access and I/O failures. Document memory ownership and error behavior.

| Operation | Required behavior |
| --- | --- |
| `ringFileCreate`, `ringFileOpen`, `ringFileClose` | Create an empty file exclusively. Open an existing file in read-only or read-write mode. Report failures without silently recreating or truncating the data. |
| `ringFileAdd` | Extract the key from the input struct. Reject a duplicate active key without changing any records. Append a new key as newest and, at capacity, evict exactly the oldest record. A previously evicted key may be reused. |
| `ringFileReadByKey` | Copy the matching record into the caller's output struct. |
| `ringFileUpdateByKey` | Replace the complete record for an existing key while preserving its insertion position. Reject a replacement whose embedded key differs from the lookup key. |
| `ringFileReadOldest`, `ringFileReadNewest` | Copy the first or last record in insertion order into the caller's output struct. |
| `ringFileIteratorBegin`, `ringFileIteratorNext`, `ringFileIteratorPrevious` | Start at the oldest or newest record or at an exact key. Traverse in either direction and copy each record into a caller-owned struct. Stop at the logical ends. Give each caller its own cursor. A successful mutation invalidates existing cursors on that handle. |
| `ringFileResize` | Increase or decrease capacity while preserving the relative order of retained records. On shrink, retain the newest records that fit. Support capacity one and reject zero. Persist the new capacity. |
| `ringFileGetInfo` | Report count, capacity, record size, schema version and storage size. |
| `ringFileSync` | Commit pending buffered changes and synchronize through the backend. Clean handles perform no write or sync. Report failures and document backend guarantees. |
| `ringFileValidate` | Provide a read-only integrity check with a result that identifies bad metadata or records. |
| `ringFileClear` | Explicitly discard all records while retaining schema and capacity. Apply the same commit and recovery rules as for other mutations. |

“Oldest” and “newest” refer to insertion order. Keys may arrive out of order, and gaps between numeric keys consume zero slots. A full ring holding keys 100, 300 and 200 in that order must hold 300, 200 and 50 after adding key 50. Updating 200 preserves its position. Shrinking that ring to two records retains 200 and 50.

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
- Skip writes for unchanged records. Use the defined key equivalence and initialized payload bytes when deciding whether a replacement is identical. Reads, validation and clean close must never generate component-level writes. Writable open may perform recovery when required.
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
//--
```

`<nr>` must be incremented at every change of this file. `<date/time>` as "DD-MM-EEYY - HH:MM"

## Deliverables and verification

1. Supply a drop-in `components/ring_file_store/` directory with source, public header and `CMakeLists.txt`. Include dependency metadata wherever an example needs a managed component.
2. Provide a README with the API and error table, binary format, transaction and recovery rules, storage and RAM estimates, and build instructions.
3. Provide an ESP32-S3 example that runs the same ring operations with LittleFS or an SD card. Use a sample meter record with a `uint64_t` timestamp key. Demonstrate create, reopen, wraparound, lookup, edit, iteration in both directions and resize.
4. Add automated tests for empty and single-slot rings, duplicates, key comparison, out-of-order insertion, wraparound, updates, iterator invalidation, schema mismatch, reopen, corruption, resizing after wraparound and insufficient space. Inject failures around each mutation's write, sync and rename boundaries and check the resulting state after reopening. Check that a recoverable interruption exposes a complete previous or new state.
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
