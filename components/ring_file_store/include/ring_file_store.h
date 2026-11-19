//--
//-- FileVersion: [6] 19-11-2026 - 15:25
//--
//-- ringFileStore - public C API of the ring_file_store component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
#ifndef RING_FILE_STORE_H
#define RING_FILE_STORE_H

//-- ring_file_store: persistent fixed-capacity ring files for ESP-IDF.
//--
//-- Records are caller-defined payload structs stored in fixed-size binary
//-- slots. Every file is timestamp-keyed: the component ALWAYS prepends two
//-- automatic key fields to the stored record, and every read returns them:
//--
//--   [ u64 timeStamp ][ u64 usedKey ][ caller payload bytes ]
//--     field 1          field 2        your struct only
//--
//-- The caller's record struct contains only the payload; it never declares
//-- or fills the key fields. "Oldest" and "newest" always refer to bucket
//-- (period) order; the period is derived from the timestamp at the
//-- granularity of the file type, and bucketId = period % capacity is the
//-- logical slot reported by ringFileIteratorGetSlot().
//--
//-- Storage model (see README.md for the full binary format):
//--   [ 96-byte header ] [ capacity + 1 slots ]
//-- Every mutation commits by writing exactly one slot image. The committed
//-- state is derived from per-slot metadata (sequence number, revision,
//-- CRC32), so an interrupted write can never destroy the previous
//-- committed state. The header is only written by create, resize and clear.
//--
//-- All functions run in task context and may block on storage I/O.

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
  //-- No active record carries the requested usedKey.
  RING_FILE_STORE_ERR_KEY_NOT_FOUND = RING_FILE_STORE_ERR_BASE + 2,
  //-- Reserved: no longer produced. Adding the usedKey of an active bucket
  //-- updates that record in place instead of failing.
  RING_FILE_STORE_ERR_DUPLICATE_KEY = RING_FILE_STORE_ERR_BASE + 3,
  //-- Iterator reached the logical end in the requested direction.
  RING_FILE_STORE_ERR_ITER_END = RING_FILE_STORE_ERR_BASE + 4,
  //-- A successful mutation invalidated this cursor; restart it.
  RING_FILE_STORE_ERR_ITER_INVALIDATED = RING_FILE_STORE_ERR_BASE + 5,
  //-- File type (schemaId string) or payload record size does not match
  //-- the expectation supplied to ringFileOpen().
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
  //-- ringFileAdd()/ringFileUpsert() add path rejected a bucket that is
  //-- not newer than the newest stored bucket (clock rewind or gap fill).
  //-- Nothing changed; use ringFileEdit().
  RING_FILE_STORE_ERR_NOT_NEWER = RING_FILE_STORE_ERR_BASE + 16,
  //-- ringFileEdit() requested a bucket outside the retained window
  //-- [newestPeriod - capacity + 1, newestPeriod]. Nothing changed.
  RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE = RING_FILE_STORE_ERR_BASE + 17,
} ringFileErr_t;

//-- File type strings (schemaId). The two-digit-year form;
//-- stored timestamps always carry the century (EEYY, fixed to "20").
#define RING_FILE_SCHEMA_YY "YY"
#define RING_FILE_SCHEMA_YYMM "YYMM"
#define RING_FILE_SCHEMA_YYMMDD "YYMMDD"
#define RING_FILE_SCHEMA_YYMMDDHH "YYMMDDhh"
#define RING_FILE_SCHEMA_YYMMDDHHMM "YYMMDDhhmm"
#define RING_FILE_SCHEMA_YYMMDDHHMMSS "YYMMDDhhmmss"

//-- Longest schemaId string ("YYMMDDhhmmss"), excluding the terminator.
#define RING_FILE_SCHEMA_MAX_LEN 12u

//-- Timestamp granularity, derived from the file type string. The period
//-- formulas (ts = the 14-digit EEYYMMDDhhmmss value, days = COMPLETE
//-- days before the date; 1970-01-01 -> 0) are fixed by the file format:
//-- YEAR    year
//-- MONTH   (year - 1) * 12 + month
//-- DAY     days
//-- HOUR    days * 24 + hour
//-- MINUTE  days * 1440 + hour * 60 + minute
//-- SECOND  days * 86400 + hour * 3600 + minute * 60 + second
//-- bucketId (the reported logical slot) is period % capacity. A partial
//-- current period never counts as a full unit: on day D the hourly
//-- period at 14:xx is D * 24 + 14 (completed hours only, not D * 24 + 24).
typedef enum
{
  RING_FILE_GRANULARITY_NONE = 0,   //-- invalid for a ring file
  RING_FILE_GRANULARITY_YEAR = 1,   //-- "YY"
  RING_FILE_GRANULARITY_MONTH = 2,  //-- "YYMM"
  RING_FILE_GRANULARITY_DAY = 3,    //-- "YYMMDD"
  RING_FILE_GRANULARITY_HOUR = 4,   //-- "YYMMDDhh"
  RING_FILE_GRANULARITY_MINUTE = 5, //-- "YYMMDDhhmm"
  RING_FILE_GRANULARITY_SECOND = 6, //-- "YYMMDDhhmmss"
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

//-- The two key fields the component always prepends to the stored record
//-- and always returns in read output buffers (field 1 first, then field
//-- 2, then the caller payload).
typedef struct
{
  uint64_t timeStamp; //-- Full timestamp, 14-digit EEYYMMDDhhmmss value
                      //-- back-filled with "00" below the file type.
  uint64_t usedKey;   //-- Timestamp truncated to the file-type digits,
                      //-- e.g. 2026111814 for type "YYMMDDhh".
} ringFileKeyFields_t;

//-- Size of the output record buffer for a payload struct type: the two
//-- automatic key fields followed by the payload.
#define RING_FILE_FULL_RECORD_SIZE(payloadType) \
  (sizeof(ringFileKeyFields_t) + sizeof(payloadType))

//-- Create configuration. schemaId and recordSize are the schema; a
//-- payload layout change requires deleting the file and creating a new
//-- one. The key fields are never part of the caller struct.
typedef struct
{
  uint32_t maxRecords;    //-- ring capacity, >= 1.
  const char *schemaId;   //-- file type, one of the RING_FILE_SCHEMA_*
                          //-- strings (case-sensitive).
  uint16_t recordSize;    //-- sizeof the caller payload struct (the two
                          //-- key fields are added by the component).
  ringFilePersistMode_t persistMode;
  //-- Buffered mode: maximum number of staged records (1..64). 0 selects
  //-- the default of 8. Ignored in immediate mode.
  uint16_t maxPendingRecords;
} ringFileConfig_t;

//-- Open configuration: the expected schema is validated against the file.
typedef struct
{
  const char *expectedSchemaId;    //-- must equal the stored file type.
  uint32_t expectedRecordSize;     //-- expected caller payload size.
  bool readOnly;                   //-- no writes, no locking, no recovery.
  ringFilePersistMode_t persistMode;
  uint16_t maxPendingRecords;
} ringFileOpenConfig_t;

//-- Snapshot for ringFileGetInfo().
typedef struct
{
  uint32_t count;                  //-- active records, including staged ones.
  uint32_t capacity;               //-- from the file, not the config.
  uint32_t payloadSize;            //-- caller record struct size.
  uint32_t recordSize;             //-- stored record bytes = payloadSize + 16.
  uint64_t storageSize;            //-- header + (capacity + 1) * slotSize.
  uint32_t pendingCount;           //-- staged, not yet committed records.
  bool dirty;                      //-- uncommitted changes outstanding.
  char schemaId[RING_FILE_SCHEMA_MAX_LEN + 1u]; //-- from the file header.
  ringFileGranularity_t granularity;            //-- from the file header.
} ringFileInfo_t;

//-- Result of ringFileValidate(). Read-only integrity inspection.
typedef enum
{
  RING_FILE_META_OK = 0,
  RING_FILE_META_BAD_MAGIC,
  RING_FILE_META_BAD_FORMAT_VERSION,
  RING_FILE_META_BAD_CHECKSUM,
  RING_FILE_META_BAD_GEOMETRY,     //-- impossible record size / capacity /
                                   //-- file type / file length.
  RING_FILE_META_SHORT_FILE,
} ringFileMetaIssue_t;

typedef struct
{
  bool headerValid;
  ringFileMetaIssue_t metadataIssue;
  uint32_t capacity;               //-- meaningful when headerValid.
  uint32_t recordSize;             //-- stored record bytes.
  uint32_t slotCount;              //-- capacity + 1 slot positions examined.
  uint32_t badSlotCount;           //-- slots with slot marker but bad CRC.
  uint32_t committedCount;         //-- active records a reopen would accept.
  bool strayResizeFiles;           //-- leftover resize candidates found.
} ringFileValidation_t;

//-- Caller-owned iteration cursor. Give every concurrent traversal its own
//-- cursor. Treat it as opaque after ringFileIteratorBegin().
typedef struct
{
  uint64_t cursorOrder;            //-- internal: period of the last returned record
  uint32_t stamp;                  //-- internal: mutation generation.
  uint32_t slot;                   //-- internal: bucketId of the last returned
                                   //-- record (RING_FILE_SLOT_STAGED for a
                                   //-- staged record).
  bool begun;                      //-- internal: begin() completed.
} ringFileIterator_t;

//-- ringFileIteratorGetSlot() reports this value when the last returned
//-- record is staged in RAM and has not reached storage yet (buffered mode).
#define RING_FILE_SLOT_STAGED 0xFFFFFFFFu

//-- Opaque handle.
typedef struct ringFileHandle_s ringFileHandle_t;

//-- Buffer contract (applies to every record and key argument):
//--  * Payload buffers (add, upsert, edit, updateByKey) are caller-owned
//--    and at least recordSize bytes — the payload struct only; the
//--    component adds the two key fields on write.
//--  * Output record buffers (all reads and iterator steps) hold at least
//--    RING_FILE_FULL_RECORD_SIZE(payloadType) bytes: ringFileKeyFields_t
//--    (timeStamp, then usedKey) followed by the payload. The component
//--    fills all of it.
//--  * Initialize all payload padding bytes before writing (for example
//--    memset the whole struct first). Unchanged-update detection compares
//--    the full stored record (key fields and padding included), so
//--    uninitialized padding breaks write skipping and reproducibility.
//--  * Timestamp arguments are numeric EEYYMMDDhhmmss uint64_t values
//--    (14 digits, century fixed to "20"). Digits below the file-type
//--    granularity are validated as zero padding or valid values but never
//--    take part in the key.
//--  * Key arguments are plain uint64_t usedKey values (the timestamp
//--    truncated to the file-type digits), e.g. 2026102317 for hour 17 on
//--    23-10-2026 in a "YYMMDDhh" file.
//--  * Payload structs must be pointer-free and contain only fixed-width
//--    fields. Payloads are stored as raw bytes: reopening requires the
//--    same payload layout, ABI and little-endian byte order. Any payload
//--    layout change requires deleting the old file and creating a new one.
//--  * Integers are stored little-endian and read without unaligned loads
//--    and without alignment requirements on the payload struct.

//-- Create an empty ring file exclusively and return it open read-write.
//-- Fails with RING_FILE_STORE_ERR_ALREADY_EXISTS if the path exists; it
//-- never recreates or truncates an existing file.
esp_err_t ringFileCreate(const char *path, const ringFileConfig_t *config,
                         ringFileHandle_t **outHandle);

//-- Open an existing ring file. The stored file type and payload size must
//-- match the expectation, otherwise RING_FILE_STORE_ERR_SCHEMA_MISMATCH.
//-- Capacity comes from the file. A read-write open may perform recovery
//-- writes. A read-only open never writes; if recovery is required it fails
//-- with RING_FILE_STORE_ERR_RECOVERY_REQUIRED.
esp_err_t ringFileOpen(const char *path, const ringFileOpenConfig_t *openConfig,
                       ringFileHandle_t **outHandle);

//-- Commit pending staged data, close the file and release the handle.
//-- A clean handle performs no write and no sync. On I/O failure the data
//-- on storage still exposes a complete previous-or-new committed state;
//-- the return value reports the failure. The pointer is invalid after
//-- this call regardless of the result.
void ringFileClose(ringFileHandle_t *handle);

//-- Append (timeStamp, payload) as newest. The component validates the
//-- timestamp for the file type, stores the full timestamp as field 1 and
//-- the derived usedKey as field 2, and computes the bucketId
//-- (period % capacity). Adding the usedKey of an active bucket updates
//-- that record in place (position preserved). A bucket not newer than
//-- the newest stored bucket is rejected with
//-- RING_FILE_STORE_ERR_NOT_NEWER without changes (use ringFileEdit).
//-- At capacity exactly the oldest bucket is evicted. Invalid timestamp
//-- digits for the file type: ESP_ERR_INVALID_ARG.
esp_err_t ringFileAdd(ringFileHandle_t *handle, uint64_t timeStamp,
                      const void *record);

//-- Copy the full stored record (timeStamp, usedKey, payload) of the
//-- active record with this usedKey into the caller's output buffer
//-- (RING_FILE_FULL_RECORD_SIZE bytes). Unknown key:
//-- RING_FILE_STORE_ERR_KEY_NOT_FOUND.
esp_err_t ringFileReadByKey(ringFileHandle_t *handle, uint64_t usedKey,
                            void *outRecord);

//-- Replace the complete record (stored timestamp and payload) for an
//-- existing usedKey and preserve its bucket position. The usedKey derived
//-- from timeStamp must equal the lookup key, otherwise ESP_ERR_INVALID_ARG.
//-- A replacement identical to the current record produces no write.
esp_err_t ringFileUpdateByKey(ringFileHandle_t *handle, uint64_t usedKey,
                              uint64_t timeStamp, const void *record);

//-- Convenience update-or-add for a logger that revises the current
//-- bucket's record: updates in place when the derived usedKey is active,
//-- otherwise appends under the ringFileAdd() rules. Lookup and mutation
//-- happen under the handle lock, so the operation is atomic with respect
//-- to other users of this handle.
esp_err_t ringFileUpsert(ringFileHandle_t *handle, uint64_t timeStamp,
                         const void *record);

//-- Backfill/edit the file when measurements for one or more buckets were
//-- lost. Updates the active record of the record's bucket for any
//-- timestamp; otherwise inserts the bucket as a new record when its
//-- period lies inside the retained window
//-- [newestPeriod - capacity + 1, newestPeriod] (an empty ring accepts
//-- any bucket; at capacity the oldest bucket is evicted). Outside the
//-- window: RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE, nothing changed.
esp_err_t ringFileEdit(ringFileHandle_t *handle, uint64_t timeStamp,
                       const void *record);

//-- Copy the first / last record in bucket order (full record, key fields
//-- included) into the caller's output buffer.
esp_err_t ringFileReadOldest(ringFileHandle_t *handle, void *outRecord);
esp_err_t ringFileReadNewest(ringFileHandle_t *handle, void *outRecord);

//-- Position a cursor and copy the starting full record into outRecord.
//-- start == RING_FILE_ITER_OLDEST/NEWEST starts at that end,
//-- RING_FILE_ITER_KEY requires a non-NULL usedKey and starts at that
//-- record (RING_FILE_STORE_ERR_KEY_NOT_FOUND when inactive). Subsequent
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
                                ringFileIterStart_t start,
                                const uint64_t *usedKey, void *outRecord);
esp_err_t ringFileIteratorNext(ringFileHandle_t *handle,
                               ringFileIterator_t *iterator, void *outRecord);
esp_err_t ringFileIteratorPrevious(ringFileHandle_t *handle,
                                   ringFileIterator_t *iterator, void *outRecord);

//-- Report the bucketId (period % capacity, the logical slot) of the
//-- record the cursor last returned, for diagnostics and test
//-- logging. Returns RING_FILE_STORE_ERR_ITER_INVALIDATED when the cursor
//-- was invalidated by a mutation; the slot of the last successful record
//-- is kept. A staged record (buffered mode, not yet flushed) reports
//-- RING_FILE_SLOT_STAGED.
esp_err_t ringFileIteratorGetSlot(ringFileHandle_t *handle,
                                  const ringFileIterator_t *iterator,
                                  uint32_t *outSlot);

//-- Change capacity, preserving the relative order of retained buckets.
//-- On shrink the newest buckets that fit are retained. newCapacity must
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

//-- Explicitly discard all records. The file type and capacity are
//-- retained. Staged data is dropped; the epoch in the header is advanced
//-- so all earlier records are hidden. Uses the same commit rules as a
//-- header write (create/resize).
esp_err_t ringFileClear(ringFileHandle_t *handle);

//-- ---------------------------------------------------------------------------
//-- Timestamp helpers (usable with or without an open ring).
//--
//-- Time basis: the local wall clock is treated as if it were UTC;
//-- daylight saving time is ignored.
//--
//-- Map a file type string to its granularity. Unknown string or NULL:
//-- ESP_ERR_INVALID_ARG.
esp_err_t ringFileGranularityFromSchemaId(const char *schemaId,
                                          ringFileGranularity_t *outGranularity);

//-- Compute the usedKey (timestamp truncated to the granularity digits)
//-- of a 14-digit EEYYMMDDhhmmss timestamp, e.g. 20261118143055 with
//-- granularity HOUR gives 2026111814. Digits below the granularity may be
//-- zero padding or valid values; fields at or above the granularity must
//-- be valid (century "20", month 01-12, day valid for the month including
//-- leap years, hour 00-23, minute/second 00-59). Anything else:
//-- ESP_ERR_INVALID_ARG.
esp_err_t ringFileTimeStampToUsedKey(uint64_t timeStamp,
                                     ringFileGranularity_t granularity,
                                     uint64_t *outUsedKey);

//-- Compute the period of a timestamp at a
//-- granularity (see ringFileGranularity_t); bucketId = period % capacity.
//-- Same validation as ringFileTimeStampToUsedKey().
esp_err_t ringFileTimeStampToPeriod(uint64_t timeStamp,
                                    ringFileGranularity_t granularity,
                                    uint64_t *outPeriod);

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

//-- Compute the period value (the bucket source) for a unix timestamp:
//-- YEAR    year
//-- MONTH   (year - 1) * 12 + month
//-- DAY     days since 1970-01-01
//-- HOUR    days * 24 + hour
//-- MINUTE  days * 1440 + hour * 60 + minute
//-- SECOND  days * 86400 + hour * 3600 + minute * 60 + second
esp_err_t ringFileUnixToBucketId(uint64_t unixSeconds,
                                 ringFileGranularity_t granularity,
                                 uint64_t *outPeriod);

//-- ISO-8601 week-numbering year and week number for display (weeks
//-- start Monday; week 1 contains the first Thursday of the year).
//-- 2025-12-29 yields year 2026, week 1.
esp_err_t ringFileUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                                uint32_t *outIsoWeek);

//-- Practical update-or-add pattern for a logger revising the current
//-- hourly record: build the payload, then
//--   err = ringFileUpsert(handle, 20261023140000ULL, &rec);
//-- In buffered mode call ringFileSync() at the hourly boundary (and at
//-- any interval below which lost data is unacceptable). See README.md for
//-- the worked examples and the documented power-loss window.

#ifdef __cplusplus
}
#endif

#endif
