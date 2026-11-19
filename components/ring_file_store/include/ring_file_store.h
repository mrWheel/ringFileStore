//--
//-- FileVersion: [3] 19-11-2026 - 09:06
//--
//-- ringFileStore - public C API of the ring_file_store component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//--
#ifndef RING_FILE_STORE_H
#define RING_FILE_STORE_H

//-- ring_file_store: persistent fixed-capacity ring files for ESP-IDF.
//--
//-- Records are caller-defined structs stored in fixed-size binary slots.
//-- One caller-defined key field inside the record identifies the record.
//-- "Oldest" and "newest" always refer to insertion order, never to key
//-- order. Keys may arrive out of order and gaps between numeric keys
//-- consume no storage.
//--
//-- Storage model (see README.md for the full binary format):
//--   [ 96-byte header ] [ capacity + 1 slots ]
//-- Every mutation commits by writing exactly one slot image. The committed
//-- state is derived from per-slot metadata (sequence number, revision,
//-- CRC32), so an interrupted write can never destroy the previous
//-- committed state. The header is only written by create, resize and clear.
//--
//-- All functions run in task context and may block on storage I/O.
//--
//-- Timestamp ring files: with config.addressing =
//-- RING_FILE_ADDRESSING_TIMEBUCKET the component derives the key
//-- (bucketId) from a uint64_t timeStamp field (unix seconds of the local
//-- wall clock treated as if UTC) at a fixed granularity (hour, day,
//-- ISO-8601 week starting Monday, month). Order, eviction and resize
//-- retention then follow bucketId; see README.md "Timestamp ring files".

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

//-- Result codes. Standard ESP-IDF codes (ESP_ERR_INVALID_ARG,
//-- ESP_ERR_NO_MEM, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND) are also
//-- used where they apply. Custom codes share one base value.
#define RING_FILE_STORE_ERR_BASE 0x5600

typedef enum
{
  //-- readOldest/readNewest called on a ring with no records.
  RING_FILE_STORE_ERR_RING_EMPTY = RING_FILE_STORE_ERR_BASE + 1,
  //-- No active record carries the requested key.
  RING_FILE_STORE_ERR_KEY_NOT_FOUND = RING_FILE_STORE_ERR_BASE + 2,
  //-- add() rejected: an active record already carries the key.
  RING_FILE_STORE_ERR_DUPLICATE_KEY = RING_FILE_STORE_ERR_BASE + 3,
  //-- Iterator reached the logical end in the requested direction.
  RING_FILE_STORE_ERR_ITER_END = RING_FILE_STORE_ERR_BASE + 4,
  //-- A successful mutation invalidated this cursor; restart it.
  RING_FILE_STORE_ERR_ITER_INVALIDATED = RING_FILE_STORE_ERR_BASE + 5,
  //-- File schema id, schema version or record size does not match the
  //-- expectation supplied to ringFileOpen().
  RING_FILE_STORE_ERR_SCHEMA_MISMATCH = RING_FILE_STORE_ERR_BASE + 6,
  //-- File format not understood: bad magic, unsupported format version or
  //-- geometry that does not match the header.
  RING_FILE_STORE_ERR_FORMAT = RING_FILE_STORE_ERR_BASE + 7,
  //-- Data failed an integrity check and recovery could not determine a
  //-- valid complete state.
  RING_FILE_STORE_ERR_CORRUPT = RING_FILE_STORE_ERR_BASE + 8,
  //-- Operation requires a writable handle.
  RING_FILE_STORE_ERR_READONLY = RING_FILE_STORE_ERR_BASE + 9,
  //-- Storage backend reported an I/O failure (short write, fsync, rename).
  RING_FILE_STORE_ERR_IO = RING_FILE_STORE_ERR_BASE + 10,
  //-- Read-only open found a state that requires writing to recover
  //-- (for example a leftover resize candidate). Reopen read-write.
  RING_FILE_STORE_ERR_RECOVERY_REQUIRED = RING_FILE_STORE_ERR_BASE + 11,
  //-- Handle is quarantined after an ambiguous I/O failure. Close and
  //-- reopen to run recovery before any further mutation.
  RING_FILE_STORE_ERR_IO_FAULTED = RING_FILE_STORE_ERR_BASE + 12,
  //-- Candidate file could not be created or filled (volume full).
  RING_FILE_STORE_ERR_STORAGE_FULL = RING_FILE_STORE_ERR_BASE + 13,
  //-- ringFileCreate() refused to overwrite an existing file.
  RING_FILE_STORE_ERR_ALREADY_EXISTS = RING_FILE_STORE_ERR_BASE + 14,
  //-- Another writable handle already holds the exclusive lock file.
  RING_FILE_STORE_ERR_BUSY = RING_FILE_STORE_ERR_BASE + 15,
  //-- TIMEBUCKET mode: ringFileAdd()/ringFileUpsert() add path rejected a
  //-- timeStamp that is not newer than the newest stored record (clock
  //-- rewind or gap fill). Nothing changed; use ringFileEdit().
  RING_FILE_STORE_ERR_NOT_NEWER = RING_FILE_STORE_ERR_BASE + 16,
  //-- TIMEBUCKET mode: ringFileEdit() requested a bucket outside the
  //-- retained window [newestBucket - capacity + 1, newestBucket].
  //-- Nothing changed.
  RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE = RING_FILE_STORE_ERR_BASE + 17,
} ringFileErr_t;

//-- Supported key field types (fixed-width, stored inside the record).
typedef enum
{
  RING_FILE_KEY_INVALID = 0,
  RING_FILE_KEY_S8 = 1,
  RING_FILE_KEY_U8 = 2,
  RING_FILE_KEY_S16 = 3,
  RING_FILE_KEY_U16 = 4,
  RING_FILE_KEY_S32 = 5,
  RING_FILE_KEY_U32 = 6,
  RING_FILE_KEY_S64 = 7,
  RING_FILE_KEY_U64 = 8,
  //-- Fixed-size byte array, compared by exact byte equality.
  RING_FILE_KEY_BYTES = 9,
  //-- Fixed-size char array holding a null-terminated, case-sensitive
  //-- string. Bytes after the first terminator are ignored. A key without
  //-- a terminator inside keySize bytes is rejected.
  RING_FILE_KEY_STRING = 10,
} ringFileKeyType_t;

//-- Key field descriptor: locates and types the key inside the record.
typedef struct
{
  uint32_t keyOffset;      //-- byte offset of the key field in the record.
  ringFileKeyType_t keyType;
  uint16_t keySize;        //-- bytes occupied by the key in the record.
                           //-- Integer types: must equal the type width.
  //-- Bitmask of significant byte positions of the key. Bit i (value 1<<i)
  //-- set means byte position i takes part in key comparisons; cleared bits
  //-- are ignored, so keys differing only in cleared positions are equal.
  //-- 0 means all positions are significant (the default). When non-zero:
  //-- keySize must be <= 64 and no bit at a position >= keySize may be set.
  //-- The mask is part of the stored schema (persisted in the file header);
  //-- changing it requires a new schemaVersion. It applies to every key
  //-- comparison: duplicate check on add, keyed lookup, update key match,
  //-- upsert and iterator-by-key start.
  uint64_t keyPositionMask;
} ringFileKeyDescriptor_t;

//-- How the identifying key of a record is determined.
typedef enum
{
  //-- Default. The caller-supplied key descriptor locates the key inside
  //-- the record; oldest/newest follow insertion order (seq).
  RING_FILE_ADDRESSING_INSERTION = 0,
  //-- Timestamp mode: the component derives a bucketId from the
  //-- uint64_t timeStamp field (unix seconds) at the configured
  //-- granularity. The key is that bucketId (U64, 8 bytes, little-endian)
  //-- and oldest/newest follow bucket order. cfg.key is ignored.
  RING_FILE_ADDRESSING_TIMEBUCKET = 1,
} ringFileAddressingMode_t;

//-- Timestamp granularity for TIMEBUCKET addressing. The bucketId
//-- formulas (ts = unix seconds) are fixed by the DSMRlogger32 baseline:
//-- HOUR   ts / 3600
//-- DAY    ts / 86400
//-- WEEK   (ts + 259200) / 604800   (boundaries at Monday 00:00, ISO-8601)
//-- MONTH  (EEYY - 1) * 12 + MM     (DSMRlogger32 month formula)
typedef enum
{
  RING_FILE_GRANULARITY_NONE = 0,   //-- only valid with INSERTION addressing
  RING_FILE_GRANULARITY_HOUR = 1,
  RING_FILE_GRANULARITY_DAY = 2,
  RING_FILE_GRANULARITY_WEEK = 3,
  RING_FILE_GRANULARITY_MONTH = 4,
} ringFileGranularity_t;

//-- Persistence mode.
typedef enum
{
  //-- Default. A successful mutation is committed to the file and
  //-- synchronized through the backend before the call returns.
  RING_FILE_PERSIST_IMMEDIATE = 0,
  //-- Mutations are staged in a bounded RAM buffer. Reads through the same
  //-- handle observe staged changes. The buffer commits when full, on
  //-- ringFileSync() and on a successful ringFileClose(). A power loss may
  //-- lose staged, unsynchronized changes.
  RING_FILE_PERSIST_BUFFERED = 1,
} ringFilePersistMode_t;

//-- Create/open configuration. recordSize and the key descriptor are the
//-- schema; changing the layout requires a new schemaVersion.
typedef struct
{
  uint32_t maxRecords;             //-- ring capacity, >= 1.
  uint32_t schemaId;               //-- caller-chosen schema identifier.
  uint32_t schemaVersion;          //-- bump on every layout change.
  uint16_t recordSize;             //-- sizeof the caller record struct.
  ringFileKeyDescriptor_t key;       //-- ignored when addressing ==
                                     //-- RING_FILE_ADDRESSING_TIMEBUCKET
  ringFilePersistMode_t persistMode;
  //-- Buffered mode: maximum number of staged records (1..64). 0 selects
  //-- the default of 8. Ignored in immediate mode.
  uint16_t maxPendingRecords;
  //-- Key derivation mode (see ringFileAddressingMode_t).
  ringFileAddressingMode_t addressing;
  //-- TIMEBUCKET mode: bucket granularity (1..4). Invalid (or NONE) with
  //-- TIMEBUCKET addressing.
  ringFileGranularity_t granularity;
  //-- TIMEBUCKET mode: byte offset of the uint64_t timeStamp field inside
  //-- the record. Must satisfy timestampOffset + 8 <= recordSize.
  uint32_t timestampOffset;
} ringFileConfig_t;

//-- Open configuration: the expected schema is validated against the file.
typedef struct
{
  uint32_t expectedSchemaId;
  uint32_t expectedSchemaVersion;
  uint32_t expectedRecordSize;     //-- must equal the stored record size.
  bool readOnly;                   //-- no writes, no locking, no recovery.
  ringFilePersistMode_t persistMode;
  uint16_t maxPendingRecords;
  //-- Expected key derivation mode; a mismatch with the stored header is
  //-- RING_FILE_STORE_ERR_SCHEMA_MISMATCH. A format-version-2 file is
  //-- INSERTION addressing regardless of its reserved bytes.
  ringFileAddressingMode_t expectedAddressing;
  ringFileGranularity_t expectedGranularity;
  uint32_t expectedTimestampOffset;
} ringFileOpenConfig_t;

//-- Snapshot for ringFileGetInfo().
typedef struct
{
  uint32_t count;                  //-- active records, including staged ones.
  uint32_t capacity;               //-- from the file, not the config.
  uint32_t recordSize;
  uint32_t schemaId;
  uint32_t schemaVersion;
  uint64_t storageSize;            //-- header + (capacity + 1) * slotSize.
  uint32_t pendingCount;           //-- staged, not yet committed records.
  bool dirty;                      //-- uncommitted changes outstanding.
  uint64_t keyPositionMask;        //-- from the file header (0 = all
                                   //-- positions significant).
  ringFileAddressingMode_t addressing;   //-- from the file header.
  ringFileGranularity_t granularity;     //-- NONE in insertion mode.
  uint32_t timestampOffset;              //-- 0 in insertion mode.
} ringFileInfo_t;

//-- Result of ringFileValidate(). Read-only integrity inspection.
typedef enum
{
  RING_FILE_META_OK = 0,
  RING_FILE_META_BAD_MAGIC,
  RING_FILE_META_BAD_FORMAT_VERSION,
  RING_FILE_META_BAD_CHECKSUM,
  RING_FILE_META_BAD_GEOMETRY,     //-- impossible record size / capacity /
                                   //-- key descriptor / file length.
  RING_FILE_META_SHORT_FILE,
} ringFileMetaIssue_t;

typedef struct
{
  bool headerValid;
  ringFileMetaIssue_t metadataIssue;
  uint32_t capacity;               //-- meaningful when headerValid.
  uint32_t recordSize;
  uint32_t slotCount;              //-- capacity + 1 slot positions examined.
  uint32_t badSlotCount;           //-- slots with slot marker but bad CRC.
  uint32_t committedCount;         //-- active records a reopen would accept.
  bool strayResizeFiles;           //-- leftover resize candidates found.
} ringFileValidation_t;

//-- Caller-owned iteration cursor. Give every concurrent traversal its own
//-- cursor. Treat it as opaque after ringFileIteratorBegin().
typedef struct
{
  uint64_t cursorSeq;              //-- internal: seq of the last returned record
                                   //-- (bucketId order value in TIMEBUCKET mode).
  uint32_t stamp;                  //-- internal: mutation generation.
  uint32_t slot;                   //-- internal: physical slot of the last
                                   //-- returned record (RING_FILE_SLOT_STAGED
                                   //-- for a staged record).
  bool begun;                      //-- internal: begin() completed.
} ringFileIterator_t;

//-- ringFileIteratorGetSlot() reports this value when the last returned
//-- record is staged in RAM and has no physical slot yet (buffered mode).
#define RING_FILE_SLOT_STAGED 0xFFFFFFFFu

//-- Opaque handle.
typedef struct ringFileHandle_s ringFileHandle_t;

//-- Buffer contract (applies to every record and key argument):
//--  * record buffers are caller-owned and at least recordSize bytes.
//--    The component reads/writes exactly recordSize bytes and never keeps
//--    a pointer to them.
//--  * Initialize all padding bytes before writing (for example memset the
//--    whole struct first). Unchanged-update detection and "identical
//--    replacement" decisions compare all recordSize bytes, so uninitialized
//--    padding breaks write skipping and reproducibility.
//--  * Key arguments are caller-owned buffers of at least the descriptor's
//--    keySize bytes. Integer keys carry the raw little-endian field bytes
//--    of exactly the type width. Byte-array keys carry exactly keySize
//--    bytes. String keys are fixed-size char arrays and must contain a
//--    null byte within keySize bytes. When a keyPositionMask is set,
//--    bytes at insignificant positions may hold any value: comparisons
//--    (duplicate check, lookup, update key match) ignore them.
//--  * Records must be pointer-free and contain only fixed-width fields.
//--    Payloads are stored as raw bytes: reopening a file requires the same
//--    payload layout, ABI and little-endian byte order. Any layout change
//--    requires an explicit schemaVersion change.
//--  * Integers are extracted and compared without unaligned loads and
//--    without alignment requirements on the record struct.

//-- Create an empty ring file exclusively and return it open read-write.
//-- Fails with RING_FILE_STORE_ERR_ALREADY_EXISTS if the path exists; it
//-- never recreates or truncates an existing file.
esp_err_t ringFileCreate(const char *path, const ringFileConfig_t *config,
                         ringFileHandle_t **outHandle);

//-- Open an existing ring file. The stored schema must match the expected
//-- schema, otherwise RING_FILE_STORE_ERR_SCHEMA_MISMATCH. Capacity comes
//-- from the file. A read-write open may perform recovery writes. A
//-- read-only open never writes; if recovery is required it fails with
//-- RING_FILE_STORE_ERR_RECOVERY_REQUIRED.
esp_err_t ringFileOpen(const char *path, const ringFileOpenConfig_t *openConfig,
                       ringFileHandle_t **outHandle);

//-- Commit pending staged data, close the file and release the handle.
//-- A clean handle performs no write and no sync. On I/O failure the data
//-- on storage still exposes a complete previous-or-new committed state;
//-- the return value reports the failure. The pointer is invalid after
//-- this call regardless of the result.
void ringFileClose(ringFileHandle_t *handle);

//-- Append record as newest. The key is extracted from the record itself.
//-- Rejects a duplicate active key without changing any record. At
//-- capacity exactly the oldest record is evicted. A previously evicted
//-- key may be added again.
esp_err_t ringFileAdd(ringFileHandle_t *handle, const void *record);

//-- Copy the active record with this key into the caller's output struct.
esp_err_t ringFileReadByKey(ringFileHandle_t *handle, const void *key,
                            void *outRecord);

//-- Replace the complete record for an existing key and preserve its
//-- insertion position. The embedded key of the replacement must equal the
//-- lookup key, otherwise ESP_ERR_INVALID_ARG. A replacement identical to
//-- the current record produces no write.
esp_err_t ringFileUpdateByKey(ringFileHandle_t *handle, const void *key,
                              const void *record);

//-- Convenience update-or-add for a logger that revises the current hour's
//-- record: updates in place when the key is active, otherwise appends.
//-- Lookup and mutation happen under the handle lock, so the operation is
//-- atomic with respect to other users of this handle.
esp_err_t ringFileUpsert(ringFileHandle_t *handle, const void *record);

//-- TIMEBUCKET addressing only (ESP_ERR_INVALID_STATE otherwise):
//-- backfill/edit the file when measurements for one or more buckets were
//-- lost. Updates the active record of the record's bucket for any
//-- timeStamp; otherwise inserts the bucket as a new record when it lies
//-- inside the retained window
//-- [newestBucket - capacity + 1, newestBucket] (an empty ring accepts
//-- any bucket; at capacity the oldest bucket is evicted). Outside the
//-- window: RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE, nothing changed.
esp_err_t ringFileEdit(ringFileHandle_t *handle, const void *record);

//-- Copy the first / last record in insertion order.
esp_err_t ringFileReadOldest(ringFileHandle_t *handle, void *outRecord);
esp_err_t ringFileReadNewest(ringFileHandle_t *handle, void *outRecord);

//-- Position a cursor and copy the starting record into outRecord.
//-- start == RING_FILE_ITER_OLDEST/NEWEST starts at that end,
//-- RING_FILE_ITER_KEY requires a non-NULL key and starts at that record
//-- (RING_FILE_STORE_ERR_KEY_NOT_FOUND when inactive). Subsequent
//-- ringFileIteratorNext()/Previous() return the following/preceding
//-- records and stop with RING_FILE_STORE_ERR_ITER_END at the logical
//-- ends. A successful mutation on the handle invalidates existing cursors
//-- (RING_FILE_STORE_ERR_ITER_INVALIDATED); call begin again.
typedef enum
{
  RING_FILE_ITER_OLDEST = 0,
  RING_FILE_ITER_NEWEST = 1,
  RING_FILE_ITER_KEY = 2,
} ringFileIterStart_t;

esp_err_t ringFileIteratorBegin(ringFileHandle_t *handle,
                                ringFileIterator_t *iterator,
                                ringFileIterStart_t start, const void *key,
                                void *outRecord);
esp_err_t ringFileIteratorNext(ringFileHandle_t *handle,
                               ringFileIterator_t *iterator, void *outRecord);
esp_err_t ringFileIteratorPrevious(ringFileHandle_t *handle,
                                   ringFileIterator_t *iterator, void *outRecord);

//-- Report the physical slot index (0 .. capacity) of the record the
//-- cursor last returned, for diagnostics and test logging. In
//-- TIMEBUCKET mode this reports the logical slot bucketId % capacity
//-- (DSMRlogger-style direct offset); physical placement stays with the
//-- crash-safe commit protocol. Returns
//-- RING_FILE_STORE_ERR_ITER_INVALIDATED when the cursor was invalidated
//-- by a mutation, and RING_FILE_STORE_ERR_ITER_END semantics do not
//-- apply (the slot of the last successful record is kept). A staged
//-- record (buffered mode, not yet flushed) reports RING_FILE_SLOT_STAGED.
esp_err_t ringFileIteratorGetSlot(ringFileHandle_t *handle,
                                  const ringFileIterator_t *iterator,
                                  uint32_t *outSlot);

//-- Change capacity, preserving the relative order of retained records.
//-- On shrink the newest records that fit are retained. newCapacity must
//-- be >= 1; capacity 1 is supported. The new capacity is persisted. The
//-- resize runs through a candidate file on the same volume, verified
//-- before an atomic switch; the committed ring is preserved on recoverable
//-- errors. Pending staged data is committed first. Temporary storage:
//-- roughly header + (retainedCount + 1) * slotSize bytes on the volume.
esp_err_t ringFileResize(ringFileHandle_t *handle, uint32_t newCapacity);

esp_err_t ringFileGetInfo(ringFileHandle_t *handle, ringFileInfo_t *outInfo);

//-- Commit pending buffered changes and synchronize through the backend.
//-- A clean handle performs no write and no sync.
esp_err_t ringFileSync(ringFileHandle_t *handle);

//-- Stand-alone read-only integrity check. Never writes. Identifies bad
//-- metadata versus bad record slots in the result.
esp_err_t ringFileValidate(const char *path, ringFileValidation_t *outValidation);

//-- Remove a lock file (<path>.lck) left behind by an interrupted run.
//-- A handle that never reached ringFileClose() (panic, abort, power loss)
//-- cannot release its lock, so every later writable open returns
//-- RING_FILE_STORE_ERR_BUSY until the lock file is removed. Call this only
//-- immediately after a device reset, when no live handle can exist on this
//-- path (e.g. at startup, before the first open). It never touches the
//-- ring file itself. ESP_OK when a lock was removed, ESP_ERR_NOT_FOUND
//-- when no lock file was present. Calling it while a handle is still open
//-- breaks that handle's protection: its close would then remove a lock
//-- acquired by someone else.
esp_err_t ringFileBreakStaleLock(const char *path);

//-- Explicitly discard all records. The schema and capacity are retained.
//-- Staged data is dropped; the epoch in the header is advanced so all
//-- earlier records are hidden. Uses the same commit rules as a header
//-- write (create/resize).
esp_err_t ringFileClear(ringFileHandle_t *handle);

//-- ---------------------------------------------------------------------------
//-- Timestamp helpers (usable with or without an open ring).
//--
//-- Time basis: the local wall clock is treated as if it were UTC
//-- (DSMRlogger32-compatible); daylight saving time is ignored.
//--
//-- Parse "EEYYMMDDHHMMSS" (exactly 14 digits) into unix seconds.
//-- Validation: century digits must be "20" (years 2000-2099), month
//-- 01-12, day valid for the month (leap years included), hour 00-23,
//-- minute 00-59, second 00-59. Anything else: ESP_ERR_INVALID_ARG.
//-- Example: "20000101000000" -> 946684800.
esp_err_t ringFileTimeStampStringToUnix(const char *timeStamp,
                                        uint64_t *outUnixSeconds);

//-- Inverse of ringFileTimeStampStringToUnix(). outTimeStamp receives 14
//-- digits plus a terminating NUL (buffer of at least 15 bytes). Values
//-- outside 2000-01-01 00:00:00 .. 2099-12-31 23:59:59 give
//-- ESP_ERR_INVALID_ARG.
esp_err_t ringFileUnixToTimeStampString(uint64_t unixSeconds,
                                        char outTimeStamp[15]);

//-- Compute the TIMEBUCKET key value (bucketId) for a timestamp:
//-- HOUR   unixSeconds / 3600
//-- DAY    unixSeconds / 86400
//-- WEEK   (unixSeconds + 259200) / 604800   (Monday 00:00 boundaries)
//-- MONTH  (EEYY - 1) * 12 + MM              (DSMRlogger32 formula)
esp_err_t ringFileUnixToBucketId(uint64_t unixSeconds,
                                 ringFileGranularity_t granularity,
                                 uint64_t *outBucketId);

//-- ISO-8601 week-numbering year and week number for display (weeks
//-- start Monday; week 1 contains the first Thursday of the year).
//-- 2025-12-29 yields year 2026, week 1.
esp_err_t ringFileUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                                uint32_t *outIsoWeek);

//-- Practical update-or-add pattern for a logger revising the current
//-- hourly record: build the record, then
//--   err = ringFileUpsert(handle, &rec);
//-- In buffered mode call ringFileSync() at the hourly boundary (and at
//-- any interval below which lost data is unacceptable). See README.md for
//-- the worked examples and the documented power-loss window.

#ifdef __cplusplus
}
#endif

#endif
