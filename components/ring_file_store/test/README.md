<!--
 FileVersion: [3] 19-11-2026 - 13:25

 ringFileStore - host test suite README.
 Copyright (C) 2026 Willem Aandewiel (mrWheel)
 SPDX-License-Identifier: GPL-3.0-or-later
-->

# Component tests

`run_tests.sh` compiles the component sources (`../src`) against the small
POSIX shims in `host/shim/` with the host C compiler and runs the test
binary. No ESP-IDF installation or hardware is needed.

The suite injects failures at every write, sync and rename boundary via
the internal file-ops table (`src/rfs_file_io.h`) and checks the resulting
state after reopening. Everything runs on the timestamp key model: every
stored record carries the automatic `[timeStamp][usedKey]` fields and
every read returns them. Covered:

* empty and single-slot rings, capacity-one rotation;
* same-bucket overwrite (two timestamps in one bucket update in place,
  position preserved), `..._ERR_NOT_NEWER` rejection of older buckets on
  add/upsert, `ringFileEdit` backfill of an active bucket with any
  timestamp, insert inside the retained window and
  `..._ERR_BUCKET_OUT_OF_RANGE` outside it (an empty ring accepts any
  bucket);
* timestamp validation per file type: month/day/hour/minute/second
  range errors, non-leap 29-February rejection versus leap-year
  acceptance, century not "20", invalid adds leave the ring unchanged;
* key derivation for all six file types ("YY".."YYMMDDhhmmss"): usedKey
  truncation and period formulas checked against independent reference
  values (DSMRlogger32 month formula, days-since-epoch day period),
  bucketId = period % capacity reported by `ringFileIteratorGetSlot`,
  and the partial-current-period rule (a period advances by exactly one
  at every roll-over; the current day contributes its completed
  sub-periods only, never a full 24 h);
* eviction and retention in bucket (period) order, including a 40-record
  fill of a capacity-16 ring walked oldest → newest;
* `ringFileUpdateByKey`: derived-key mismatch → `ESP_ERR_INVALID_ARG`,
  unknown key → `..._ERR_KEY_NOT_FOUND`, unchanged replacement produces
  zero writes (I/O statistics), position preserved after update;
* iterators in both directions, key start, `..._ERR_ITER_END`,
  invalidation by successful mutations and slot reporting (including the
  staged marker in buffered mode);
* schema mismatch on reopen: wrong file type string, wrong payload size
  and NULL expectation, for three independent rings ("YYMMDDhh", "YYMM",
  "YYMMDDhhmmss");
* corruption: damaged header (magic, repaired-CRC format-version and
  granularity field pokes, header checksum), damaged slot records, and
  `ringFileValidate` reporting (metadata issue, bad slot count, committed
  count, stray resize files);
* failure injection around add/clear writes, buffered flush writes, and
  resize candidate creation, writes, fsync and both renames — every
  reopened state is the complete previous or new state; a leftover
  `.rfsnew` is removed on the next read-write open;
* busy lock and stale lock breaking, read-only guarantees (no writes at
  all), clear plus reuse, resize 5→3→6 with oldest-evicted retention;
* wear accounting: exactly one write per immediate add, zero writes for
  unchanged updates, buffered staging/overflow flushing and clean sync;
* time helpers: `"EEYYMMDDHHMMSS"` ↔ unix seconds (known value
  2000-01-01 → 946684800, invalid digits/century), `ringFileUnixToBucketId`
  for all six granularities and ISO week numbering (2025-12-29 →
  2026-W01).

To run:

```sh
./run_tests.sh
```

The run also writes `testLog.md` to the repository root: for every logged
command the **Command** (with its data) and its **Result**, the ring
dumps with timeStamp, usedKey and reported bucketId per record, and a
per-test overview table.

## Device (example) test run

`run_device_test.sh` builds the ESP32-S3 example, flashes it and captures
the demo run from the serial console into `testLog.md` in the repository
root (the same file the host suite writes — the last run wins):

```sh
./run_device_test.sh                 # build + flash + capture
./run_device_test.sh --no-flash      # reset the board, capture only
./run_device_test.sh /dev/tty.usbserial-1430
```

The script needs the ESP-IDF environment (it sources `export.sh` itself)
and `pyserial`. It resets the target, waits for the `example finished`
marker (90 s timeout) and reports PASS/FAIL; the captured serial output
is stored verbatim in a fenced block.

A future target-side Unity app (on-hardware LittleFS/FATFS runs) can be
added under this directory; it is out of scope for this session.
