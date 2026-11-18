# Component tests

`run_tests.sh` compiles the component sources (`../src`) against the small
POSIX shims in `host/shim/` with the host C compiler and runs the test
binary. No ESP-IDF installation or hardware is needed.

The suite injects failures at every write, sync and rename boundary via
the internal file-ops table (`src/rfs_file_io.h`) and checks the resulting
state after reopening. Covered, per the project prompt:

* empty and single-slot rings, capacity-one edit rotation;
* duplicates, key reuse after eviction, all key types (signed/unsigned
  8..64-bit integers, byte arrays, strings including terminator rules);
* out-of-order insertion and wraparound (spec scenario 100/300/200/50),
  updates preserving position, resize after wraparound (retain newest
  200/50), shrinking to capacity one;
* iterators in both directions and invalidation by successful mutations;
* schema mismatch on reopen, reopen persistence;
* corruption: damaged header (magic/CRC), damaged slots (CRC), truncated
  and oversized files, and `ringFileValidate` reporting;
* failure injection around add/update/clear writes and syncs plus resize
  candidate creation, writes, fsync, and both renames (including rollback
  failure) — every reopened state is the complete previous or new state;
* volume-full during resize (original preserved), busy lock, read-only
  guarantees (no writes at all), multiple independent files;
* wear accounting: one write + one sync per mutation, zero writes for
  unchanged updates, buffered coalescing and forced flush at the buffer
  limit, clean close performs no write or sync;
* `moreRecords`: a capacity-10 ring filled and wrapped with 12 records,
  with per-record key and physical slot number logged (via
  `ringFileIteratorGetSlot`);
* `keyPositionMask`: significant key positions — duplicate detection,
  keyed lookup, update key match and mask persistence/reopen, plus
  rejection of invalid masks.

To run:

```sh
./run_tests.sh
```

The run also writes `testLog.md` to the repository root: for every logged
command the **Command** (with its data) and its **Result**, the ring
dumps with key and slot number per record, and a per-test overview table.

A future target-side Unity app (on-hardware LittleFS/FATFS runs) can be
added under this directory; it is out of scope for this session because
flashing is done manually by the developer.
