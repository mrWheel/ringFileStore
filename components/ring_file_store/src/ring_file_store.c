//--
//-- FileVersion: [5] 19-11-2026 - 13:25
//--
//-- ringFileStore - main ring_file_store component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
//---------------------------------------------------------------------
//--
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ring_file_store.h"
#include "rfs_file_io.h"
#include "rfs_time.h"

static const char *TAG = "ring_file_store";

//-- ---------------------------------------------------------------------------
//-- File format (little-endian, explicitly encoded, never a mapped struct)
//--
//-- [0 .. 96)                    header, one block, written by create,
//--                              resize and clear only
//-- [96 .. 96 + slotSize * (capacity + 1))   fixed-size slots
//--
//-- The extra slot beyond capacity is the scratch area that keeps at least
//-- one slot free for the in-flight commit copy, so no committed record is
//-- ever overwritten in place. A slot is committed when it carries the
//-- highest (seq, rev) among the valid copies of its key and fewer than
//-- capacity members have a higher period (bucket order). File growth is
//-- lazy: a slot that was never written reads back as empty (short read).
//--
//-- Every stored record carries two automatic key fields in front of the
//-- caller payload:
//--   [ 0 .. 8 )  u64 timeStamp  (full 14-digit EEYYMMDDhhmmss value)
//--   [ 8 .. 16 ) u64 usedKey    (timestamp truncated to the file type)
//--   [ 16 .. )   caller payload bytes
//-- The key is the usedKey; order/eviction follow the period derived from
//-- the full timestamp.
//--
//-- Header (96 bytes, format version 4):
//--   0  u32 magic 'RFS1'      4  u16 formatVersion   6  u16 headerSize
//--   8  u64 generation        16 16 bytes schemaId string (NUL-terminated,
//--                             max 12 chars, rest 0xFF)
//--  32  u32 recordSize        36 u32 capacity        40 u32 granularity
//--  44  u32 flags             48 u64 epoch
//--  56  32 bytes reserved (0xFF)
//--  88  u32 crc32 over [0,88)
//--
//-- Slot header (20 bytes), followed by recordSize raw record bytes:
//--   0  u32 slotMagic 'RFS2'  4  u64 seq            12 u32 rev
//--  16  u32 crc32 over bytes [4,16) plus the record bytes
//-- ---------------------------------------------------------------------------

#define RFS_MAGIC 0x52465331u
#define RFS_SLOT_MAGIC 0x52465332u
//-- Format version 4 introduces the timestamp key model: automatic key
//-- fields, the schemaId string header and the period-ordered member set.
//-- Older format versions (insertion/TIMEBUCKET addressing) are rejected.
#define RFS_FORMAT_VERSION 4u
#define RFS_FORMAT_VERSION_MIN 4u
#define RFS_HEADER_SIZE 96u
#define RFS_SLOT_HDR_SIZE 20u
//-- Bytes of the two automatic key fields in front of the payload.
#define RFS_KEY_FIELDS_SIZE 16u

#define RFS_MAX_PAYLOAD_SIZE 4096u
#define RFS_MAX_RECORD_SIZE (RFS_KEY_FIELDS_SIZE + RFS_MAX_PAYLOAD_SIZE)
#define RFS_HEADER_NAME_SIZE 16u
#define RFS_MAX_CAPACITY 65536u
#define RFS_MAX_PENDING_LIMIT 64u
#define RFS_DEFAULT_MAX_PENDING 8u

//-- crc32: reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final xor.
//-- Implemented bit-by-bit; record payloads are small and bounded.
static uint32_t rfsCrc32Update(uint32_t state, const uint8_t *data, size_t len)
{
  for (size_t i = 0; i < len; i++)
  {
    state ^= data[i];

    for (int bit = 0; bit < 8; bit++)
    {
      if (state & 1u)
      {
        state = (state >> 1) ^ 0xEDB88320u;
      }
      else
      {
        state >>= 1;
      }
    }
  }

  return state;
}

static uint32_t rfsCrc32(const uint8_t *data, size_t len)
{
  return ~rfsCrc32Update(0xFFFFFFFFu, data, len);
}

static uint32_t rfsCrc32Two(const uint8_t *a, size_t lenA, const uint8_t *b, size_t lenB)
{
  uint32_t state = rfsCrc32Update(0xFFFFFFFFu, a, lenA);
  return ~rfsCrc32Update(state, b, lenB);
}

//-- Little-endian field helpers. All metadata is encoded explicitly so the
//-- on-disk layout never depends on compiler struct padding.
static void rfsPutU16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
}

static void rfsPutU32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void rfsPutU64(uint8_t *p, uint64_t v)
{
  rfsPutU32(p, (uint32_t)v);
  rfsPutU32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t rfsGetU16(const uint8_t *p)
{
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rfsGetU32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rfsGetU64(const uint8_t *p)
{
  return (uint64_t)rfsGetU32(p) | ((uint64_t)rfsGetU32(p + 4) << 32);
}

//-- ---------------------------------------------------------------------------
//-- Backend indirection. Every storage call goes through these wrappers so
//-- the host test suite can inject failures at write / sync / rename
//-- boundaries via rfs_file_io.h.
//-- ---------------------------------------------------------------------------

static rfsIoHooks_t sIoHooks;
static rfsIoStats_t sIoStats;

void ringFileStoreSetIoHooks(const rfsIoHooks_t *hooks)
{
  if (hooks == NULL)
  {
    memset(&sIoHooks, 0, sizeof(sIoHooks));
  }
  else
  {
    sIoHooks = *hooks;
  }
}

void ringFileStoreGetIoStats(rfsIoStats_t *outStats)
{
  if (outStats != NULL)
  {
    *outStats = sIoStats;
  }
}

void ringFileStoreResetIoStats(void)
{
  memset(&sIoStats, 0, sizeof(sIoStats));
}

static int rfsIoOpen(const char *path, int flags, int mode)
{
  return sIoHooks.open != NULL ? sIoHooks.open(path, flags, mode)
                               : open(path, flags, (mode_t)mode);
}

static int rfsIoClose(int fd)
{
  return sIoHooks.close != NULL ? sIoHooks.close(fd) : close(fd);
}

static ssize_t rfsIoRead(int fd, void *buf, size_t count)
{
  sIoStats.reads++;
  return sIoHooks.read != NULL ? sIoHooks.read(fd, buf, count) : read(fd, buf, count);
}

static ssize_t rfsIoWrite(int fd, const void *buf, size_t count)
{
  sIoStats.writes++;
  sIoStats.writeBytes += (uint32_t)count;
  return sIoHooks.write != NULL ? sIoHooks.write(fd, buf, count) : write(fd, buf, count);
}

static off_t rfsIoLseek(int fd, off_t offset, int whence)
{
  return sIoHooks.lseek != NULL ? sIoHooks.lseek(fd, offset, whence)
                                : lseek(fd, offset, whence);
}

static int rfsIoFsync(int fd)
{
  sIoStats.fsyncs++;
  return sIoHooks.fsync != NULL ? sIoHooks.fsync(fd) : fsync(fd);
}

static int rfsIoRename(const char *fromPath, const char *toPath)
{
  sIoStats.renames++;
  return sIoHooks.rename != NULL ? sIoHooks.rename(fromPath, toPath)
                                 : rename(fromPath, toPath);
}

static int rfsIoUnlink(const char *path)
{
  sIoStats.unlinks++;
  return sIoHooks.unlink != NULL ? sIoHooks.unlink(path) : unlink(path);
}

static esp_err_t rfsErrFromErrno(void)
{
  switch (errno)
  {
    case ENOENT:
      return ESP_ERR_NOT_FOUND;
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
      return RING_FILE_STORE_ERR_STORAGE_FULL;
    case ENOMEM:
      return ESP_ERR_NO_MEM;
    default:
      return RING_FILE_STORE_ERR_IO;
  }
}

//-- ---------------------------------------------------------------------------
//-- Handle internals
//-- ---------------------------------------------------------------------------

typedef struct
{
  uint64_t seq;
  uint32_t rev;
  int32_t slotIndex;  //-- -1 while not yet written to storage
  bool used;
} rfsPendingEntry_t;

struct ringFileHandle_s
{
  int fd;
  int lockFd;
  char *path;
  char *lockPath;
  char *oldPath;
  char *newPath;
  bool readOnly;
  bool dirty;
  bool ioFaulted;
  bool lockHeld;
  bool ownsLock;
  SemaphoreHandle_t lock;
  uint32_t mutationStamp;

  //-- header mirror
  uint64_t generation;
  uint64_t epoch;
  char schemaId[RING_FILE_SCHEMA_MAX_LEN + 1u];
  uint32_t granularity;      //-- ringFileGranularity_t
  uint32_t payloadSize;      //-- caller struct bytes (key fields excluded)
  uint32_t recordSize;       //-- stored bytes = payloadSize + 16
  uint32_t capacity;
  uint32_t slotSize;

  uint64_t nextSeq;
  uint32_t count;
  uint32_t nextSlotHint;

  //-- bounded staged overlay
  uint32_t maxPending;
  uint32_t pendingCount;
  rfsPendingEntry_t *pending;
  uint8_t *pendingRecords;
  bool pendingEpochChange;
  uint64_t stagedEpoch;

  //-- scratch. keyBufA is internal to rfsBestCopy(); callers that hold a
  //-- candidate key across a rfsBestCopy() call must pass keyBufB. Each key
  //-- buffer holds the 8-byte little-endian usedKey. recBuf is the build
  //-- buffer for the public write calls ([timeStamp][usedKey][payload]).
  uint8_t *slotBuf;
  uint8_t *keyBufA;
  uint8_t *keyBufB;
  uint8_t *recBuf;
};

typedef struct
{
  bool valid;
  uint64_t seq;
  uint32_t rev;
} rfsSlotHdr_t;

typedef struct
{
  bool found;
  uint64_t seq;
  uint32_t rev;
  int32_t slot;
  bool pending;
  uint32_t pendingIndex;
  uint64_t order;            //-- period of the member (bucket order value).
} rfsMember_t;

#define RFS_ENTER(h)                          \
  do                                          \
  {                                           \
    if ((h)->lock != NULL)                    \
    {                                         \
      xSemaphoreTake((h)->lock, portMAX_DELAY); \
    }                                         \
  } while (0)

#define RFS_EXIT(h)              \
  do                             \
  {                              \
    if ((h)->lock != NULL)       \
    {                            \
      xSemaphoreGive((h)->lock); \
    }                            \
  } while (0)

static void rfsEncodeHeader(const ringFileHandle_t *h, uint8_t *buf,
                            uint64_t generation, uint64_t epoch,
                            uint32_t capacity);

static esp_err_t rfsUpdateLocked(ringFileHandle_t *handle, const uint8_t *key,
                                 const uint8_t *record);

static char *rfsStrdup(const char *s)
{
  size_t len = strlen(s) + 1;
  char *copy = (char *)malloc(len);

  if (copy != NULL)
  {
    memcpy(copy, s, len);
  }

  return copy;
}

static char *rfsStrdupWithSuffix(const char *s, const char *suffix)
{
  size_t len = strlen(s) + strlen(suffix) + 1;
  char *copy = (char *)malloc(len);

  if (copy != NULL)
  {
    snprintf(copy, len, "%s%s", s, suffix);
  }

  return copy;
}

static uint64_t rfsSlotOffset(const ringFileHandle_t *h, uint32_t index)
{
  return (uint64_t)RFS_HEADER_SIZE + (uint64_t)index * h->slotSize;
}

static uint32_t rfsSlotCount(const ringFileHandle_t *h)
{
  return h->capacity + 1u;
}

static void rfsFreeHandleMembers(ringFileHandle_t *h)
{
  free(h->path);
  free(h->lockPath);
  free(h->oldPath);
  free(h->newPath);
  free(h->pending);
  free(h->pendingRecords);
  free(h->slotBuf);
  free(h->keyBufA);
  free(h->keyBufB);
  free(h->recBuf);

  if (h->ownsLock && h->lock != NULL)
  {
    vSemaphoreDelete(h->lock);
  }
}

static esp_err_t rfsAllocScratch(ringFileHandle_t *h)
{
  if (h->slotBuf != NULL)
  {
    return ESP_OK;
  }

  h->slotBuf = (uint8_t *)malloc(h->slotSize);
  h->keyBufA = (uint8_t *)malloc(8);
  h->keyBufB = (uint8_t *)malloc(8);
  h->recBuf = (uint8_t *)malloc(h->recordSize);

  if (h->slotBuf == NULL || h->keyBufA == NULL || h->keyBufB == NULL ||
      h->recBuf == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

//-- ---------------------------------------------------------------------------
//-- Positional I/O with full short-operation checking
//-- ---------------------------------------------------------------------------

static bool rfsReadAt(int fd, uint64_t offset, uint8_t *buf, size_t len, bool *outEof)
{
  *outEof = false;

  if (len == 0)
  {
    return true;
  }

  if (rfsIoLseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset)
  {
    return false;
  }

  size_t done = 0;

  while (done < len)
  {
    ssize_t n = rfsIoRead(fd, buf + done, len - done);

    if (n < 0)
    {
      return false;
    }

    if (n == 0)
    {
      //-- Short read: lazily grown file (empty region) or backend error.
      *outEof = true;
      return false;
    }

    done += (size_t)n;
  }

  return true;
}

static esp_err_t rfsWriteAt(ringFileHandle_t *h, int fd, uint64_t offset,
                            const uint8_t *buf, size_t len)
{
  if (rfsIoLseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset)
  {
    h->ioFaulted = true;
    return RING_FILE_STORE_ERR_IO;
  }

  size_t done = 0;

  while (done < len)
  {
    ssize_t n = rfsIoWrite(fd, buf + done, len - done);

    if (n <= 0)
    {
      //-- Ambiguous: some prefix may have reached storage. Quarantine.
      h->ioFaulted = true;
      return RING_FILE_STORE_ERR_IO;
    }

    done += (size_t)n;
  }

  return ESP_OK;
}

static esp_err_t rfsSyncFd(ringFileHandle_t *h, int fd)
{
  if (rfsIoFsync(fd) != 0)
  {
    h->ioFaulted = true;
    return RING_FILE_STORE_ERR_IO;
  }

  return ESP_OK;
}

static int64_t rfsFileLength(int fd)
{
  off_t end = rfsIoLseek(fd, 0, SEEK_END);

  if (end < 0)
  {
    return -1;
  }

  return (int64_t)end;
}

//-- ---------------------------------------------------------------------------
//-- Key handling. Every stored record carries the automatic key fields
//-- [u64 timeStamp][u64 usedKey] in front of the payload. The key is the
//-- usedKey, compared as raw little-endian u64 field bytes (value equality
//-- on the supported little-endian targets); extraction uses rfsGetU64 so
//-- no unaligned load is ever issued. Order, eviction and resize retention
//-- follow the period ("berekende waarde") derived from the full timestamp
//-- at the file-type granularity.
//-- ---------------------------------------------------------------------------

static uint64_t rfsTimeStampOfRecord(const uint8_t *record)
{
  return rfsGetU64(record);
}

static uint64_t rfsKeyOfRecord(const uint8_t *record)
{
  return rfsGetU64(record + 8u);
}

static bool rfsKeyEqual(const uint8_t *a, const uint8_t *b)
{
  return memcmp(a, b, 8) == 0;
}

static void rfsExtractKey(const ringFileHandle_t *h, const uint8_t *record,
                          uint8_t *keyOut)
{
  (void)h;
  rfsPutU64(keyOut, rfsKeyOfRecord(record));
}

//-- Period of a stored record (from the full timestamp). The digits were
//-- validated on write and the CRC covers the bytes, so the conversion
//-- always succeeds for a valid slot.
static uint64_t rfsOrderOfRecord(const ringFileHandle_t *h,
                                 const uint8_t *record)
{
  uint64_t period = 0;
  (void)rfsTimeDigitsToPeriod(rfsTimeStampOfRecord(record),
                              (ringFileGranularity_t)h->granularity, &period);
  return period;
}

//-- Period behind a key (usedKey) byte buffer: zero-pads the usedKey back
//-- to 14 digits, then applies the period formula.
static uint64_t rfsOrderOfKey(const ringFileHandle_t *h, const uint8_t *key)
{
  uint64_t period = 0;
  (void)rfsTimeUsedKeyToPeriod(rfsGetU64(key),
                               (ringFileGranularity_t)h->granularity, &period);
  return period;
}

//-- Period of a staged (pending) record.
static uint64_t rfsOrderOfPending(const ringFileHandle_t *h, uint32_t p)
{
  return rfsOrderOfRecord(h, h->pendingRecords + (size_t)p * h->recordSize);
}

//-- Slot index reported by ringFileIteratorGetSlot(): the DSMRlogger-style
//-- logical slot bucketId = period % capacity, and the staged marker for a
//-- record that has not reached storage yet.
static uint32_t rfsSlotReportOf(const ringFileHandle_t *h, const rfsMember_t *m)
{
  if (m->pending)
  {
    return RING_FILE_SLOT_STAGED;
  }

  return (uint32_t)(m->order % h->capacity);
}

//-- Build one stored record image [timeStamp][usedKey][payload] from a
//-- validated timestamp and a caller payload buffer. The caller holds the
//-- handle lock. outOrder receives the period of the record.
static esp_err_t rfsBuildRecordLocked(ringFileHandle_t *h, uint64_t timeStamp,
                                      const void *payload, uint8_t *outBuf,
                                      uint64_t *outOrder)
{
  uint64_t usedKey = 0;

  if (!rfsTimeDigitsToUsedKey(timeStamp, (ringFileGranularity_t)h->granularity,
                              &usedKey))
  {
    return ESP_ERR_INVALID_ARG;
  }

  rfsPutU64(outBuf, timeStamp);
  rfsPutU64(outBuf + 8u, usedKey);
  memcpy(outBuf + RFS_KEY_FIELDS_SIZE, payload, h->payloadSize);

  uint64_t period = 0;
  (void)rfsTimeDigitsToPeriod(timeStamp, (ringFileGranularity_t)h->granularity,
                              &period);

  if (outOrder != NULL)
  {
    *outOrder = period;
  }

  return ESP_OK;
}

//-- ---------------------------------------------------------------------------
//-- Slot access
//-- ---------------------------------------------------------------------------

static bool rfsSlotInspect(ringFileHandle_t *h, uint32_t index, rfsSlotHdr_t *out,
                           uint8_t *recordOut, bool *outBad)
{
  uint8_t raw[RFS_SLOT_HDR_SIZE];
  bool eof = false;

  out->valid = false;

  if (outBad != NULL)
  {
    *outBad = false;
  }

  if (!rfsReadAt(h->fd, rfsSlotOffset(h, index), raw, RFS_SLOT_HDR_SIZE, &eof))
  {
    return false;
  }

  if (rfsGetU32(raw) != RFS_SLOT_MAGIC)
  {
    return false;
  }

  uint64_t seq = rfsGetU64(raw + 4);
  uint32_t rev = rfsGetU32(raw + 12);
  uint32_t crc = rfsGetU32(raw + 16);
  uint8_t *record = (recordOut != NULL) ? recordOut : h->slotBuf;

  if (!rfsReadAt(h->fd, rfsSlotOffset(h, index) + RFS_SLOT_HDR_SIZE, record,
                 h->recordSize, &eof))
  {
    if (outBad != NULL)
    {
      *outBad = true;
    }

    return false;
  }

  uint32_t calc = rfsCrc32Two(raw + 4, 12, record, h->recordSize);

  if (calc != crc)
  {
    if (outBad != NULL)
    {
      *outBad = true;
    }

    return false;
  }

  if (seq < h->epoch)
  {
    //-- Hidden by ringFileClear().
    return false;
  }

  out->valid = true;
  out->seq = seq;
  out->rev = rev;
  return true;
}

static bool rfsSlotHeaderValid(ringFileHandle_t *h, uint32_t index, rfsSlotHdr_t *out)
{
  return rfsSlotInspect(h, index, out, NULL, NULL);
}

//-- Build and commit one slot image (header + record) in a single write so
//-- a torn write can only ever produce a slot that fails its own CRC.
static esp_err_t rfsWriteSlotImage(ringFileHandle_t *h, int fd, uint32_t index,
                                   uint64_t seq, uint32_t rev,
                                   const uint8_t *record)
{
  uint8_t raw[RFS_SLOT_HDR_SIZE];

  rfsPutU32(raw, RFS_SLOT_MAGIC);
  rfsPutU64(raw + 4, seq);
  rfsPutU32(raw + 12, rev);
  rfsPutU32(raw + 16, rfsCrc32Two(raw + 4, 12, record, h->recordSize));

  memcpy(h->slotBuf, raw, RFS_SLOT_HDR_SIZE);
  memcpy(h->slotBuf + RFS_SLOT_HDR_SIZE, record, h->recordSize);

  return rfsWriteAt(h, fd, rfsSlotOffset(h, index), h->slotBuf, h->slotSize);
}

//-- ---------------------------------------------------------------------------
//-- Committed-state derivation (scan based, bounded RAM)
//-- ---------------------------------------------------------------------------

static bool rfsMemberBetter(uint64_t seqA, uint32_t revA, uint64_t seqB, uint32_t revB)
{
  return (seqA > seqB) || (seqA == seqB && revA > revB);
}

//-- Newest valid copy of a key across disk slots and staged entries. The
//-- key is the 8-byte little-endian usedKey; out->order carries the period
//-- behind that key.
static void rfsBestCopy(ringFileHandle_t *h, const uint8_t *key, rfsMember_t *out)
{
  out->found = false;
  out->seq = 0;
  out->rev = 0;
  out->slot = -1;
  out->pending = false;
  out->pendingIndex = 0;
  out->order = rfsOrderOfKey(h, key);

  uint32_t slots = rfsSlotCount(h);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotInspect(h, idx, &hdr, NULL, NULL))
    {
      continue;
    }

    rfsExtractKey(h, h->slotBuf, h->keyBufA);

    if (!rfsKeyEqual(h->keyBufA, key))
    {
      continue;
    }

    if (!out->found || rfsMemberBetter(hdr.seq, hdr.rev, out->seq, out->rev))
    {
      out->found = true;
      out->seq = hdr.seq;
      out->rev = hdr.rev;
      out->slot = (int32_t)idx;
      out->pending = false;
    }
  }

  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (!h->pending[p].used)
    {
      continue;
    }

    const uint8_t *record = h->pendingRecords + (size_t)p * h->recordSize;
    rfsExtractKey(h, record, h->keyBufA);

    if (!rfsKeyEqual(h->keyBufA, key))
    {
      continue;
    }

    if (!out->found ||
        rfsMemberBetter(h->pending[p].seq, h->pending[p].rev, out->seq, out->rev))
    {
      out->found = true;
      out->seq = h->pending[p].seq;
      out->rev = h->pending[p].rev;
      out->slot = -1;
      out->pending = true;
      out->pendingIndex = p;
    }
  }
}

//-- Number of members (distinct keys, newest copies) whose period is
//-- greater than the given one. A member is committed when this rank is
//-- < capacity.
static uint32_t rfsRankGreater(ringFileHandle_t *h, uint64_t order)
{
  uint32_t rank = 0;
  uint32_t slots = rfsSlotCount(h);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(h, idx, &hdr))
    {
      continue;
    }

    rfsExtractKey(h, h->slotBuf, h->keyBufB);
    uint64_t slotOrder = rfsOrderOfRecord(h, h->slotBuf);

    if (slotOrder <= order)
    {
      continue;
    }

    rfsMember_t best;
    rfsBestCopy(h, h->keyBufB, &best);

    if (best.found && !best.pending && (uint32_t)best.slot == idx)
    {
      rank++;
    }
  }

  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (!h->pending[p].used)
    {
      continue;
    }

    uint64_t pOrder = rfsOrderOfPending(h, p);

    if (pOrder <= order)
    {
      continue;
    }

    rfsExtractKey(h, h->pendingRecords + (size_t)p * h->recordSize, h->keyBufB);

    rfsMember_t best;
    rfsBestCopy(h, h->keyBufB, &best);

    if (best.found && best.pending && best.pendingIndex == p)
    {
      rank++;
    }
  }

  return rank;
}

static bool rfsMemberIsCommitted(ringFileHandle_t *h, const rfsMember_t *m)
{
  if (!m->found)
  {
    return false;
  }

  return rfsRankGreater(h, m->order) < h->capacity;
}

static bool rfsFindActive(ringFileHandle_t *h, const uint8_t *key, rfsMember_t *out)
{
  rfsBestCopy(h, key, out);

  if (!out->found || !rfsMemberIsCommitted(h, out))
  {
    out->found = false;
    return false;
  }

  return true;
}

static esp_err_t rfsMemberLoad(ringFileHandle_t *h, const rfsMember_t *m,
                               uint8_t *out)
{
  if (m->pending)
  {
    memcpy(out, h->pendingRecords + (size_t)m->pendingIndex * h->recordSize,
           h->recordSize);
    return ESP_OK;
  }

  rfsSlotHdr_t hdr;

  if (!rfsSlotInspect(h, (uint32_t)m->slot, &hdr, out, NULL))
  {
    return RING_FILE_STORE_ERR_CORRUPT;
  }

  return ESP_OK;
}

static uint32_t rfsCountActive(ringFileHandle_t *h)
{
  uint32_t distinct = 0;
  uint32_t slots = rfsSlotCount(h);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(h, idx, &hdr))
    {
      continue;
    }

    rfsExtractKey(h, h->slotBuf, h->keyBufB);

    rfsMember_t best;
    rfsBestCopy(h, h->keyBufB, &best);

    if (best.found && !best.pending && (uint32_t)best.slot == idx)
    {
      distinct++;
    }
  }


  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (h->pending[p].used)
    {
      distinct++;
    }
  }

  return (distinct > h->capacity) ? h->capacity : distinct;
}

//-- Choose a slot that carries no committed member (never-used, corrupted,
//-- stale copy or evicted record). The capacity + 1 layout guarantees one
//-- exists while membership stays <= capacity.
static esp_err_t rfsSelectFreeSlot(ringFileHandle_t *h, uint32_t *outIdx)
{
  uint32_t slots = rfsSlotCount(h);

  for (uint32_t k = 0; k < slots; k++)
  {
    uint32_t idx = (h->nextSlotHint + k) % slots;
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(h, idx, &hdr))
    {
      *outIdx = idx;
      return ESP_OK;
    }

    rfsExtractKey(h, h->slotBuf, h->keyBufB);
    uint64_t slotOrder = rfsOrderOfRecord(h, h->slotBuf);

    rfsMember_t best;
    rfsBestCopy(h, h->keyBufB, &best);

    bool isMember = best.found && !best.pending && (uint32_t)best.slot == idx &&
                    rfsRankGreater(h, slotOrder) < h->capacity;

    if (!isMember)
    {
      *outIdx = idx;
      return ESP_OK;
    }
  }

  ESP_LOGE(TAG, "no free slot: invariant violation");
  return ESP_ERR_INVALID_STATE;
}

//-- ---------------------------------------------------------------------------
//-- Staged overlay (buffered persistence)
//-- ---------------------------------------------------------------------------

static int32_t rfsFindPending(ringFileHandle_t *h, const uint8_t *key)
{
  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (!h->pending[p].used)
    {
      continue;
    }

    rfsExtractKey(h, h->pendingRecords + (size_t)p * h->recordSize, h->keyBufA);

    if (rfsKeyEqual(h->keyBufA, key))
    {
      return (int32_t)p;
    }
  }

  return -1;
}

static esp_err_t rfsFlushPending(ringFileHandle_t *h, bool doSync)
{
  if (h->pendingCount == 0 && !h->dirty && !h->pendingEpochChange)
  {
    //-- Clean handle: no write, no sync.
    return ESP_OK;
  }

  //-- Commit staged slot images in insertion order.
  uint32_t remaining = 0;

  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (h->pending[p].used && h->pending[p].slotIndex < 0)
    {
      remaining++;
    }
  }

  while (remaining > 0)
  {
    int32_t lowest = -1;

    for (uint32_t p = 0; p < h->pendingCount; p++)
    {
      if (h->pending[p].used && h->pending[p].slotIndex < 0 &&
          (lowest < 0 || h->pending[p].seq < h->pending[lowest].seq))
      {
        lowest = (int32_t)p;
      }
    }

    if (lowest < 0)
    {
      break;
    }

    uint32_t idx = 0;
    esp_err_t err = rfsSelectFreeSlot(h, &idx);

    if (err != ESP_OK)
    {
      return err;
    }

    const uint8_t *record =
        h->pendingRecords + (size_t)lowest * h->recordSize;
    err = rfsWriteSlotImage(h, h->fd, idx, h->pending[lowest].seq,
                            h->pending[lowest].rev, record);

    if (err != ESP_OK)
    {
      return err;
    }

    h->pending[lowest].slotIndex = (int32_t)idx;
    h->nextSlotHint = (idx + 1u) % rfsSlotCount(h);
    remaining--;
  }

  esp_err_t err = ESP_OK;

  if (h->pendingEpochChange)
  {
    //-- Commit the clear: advance the epoch in the header. This is the only
    //-- path besides create and resize that touches the header block.
    uint64_t previousEpoch = h->epoch;
    uint8_t buf[RFS_HEADER_SIZE];
    h->epoch = h->stagedEpoch;
    rfsEncodeHeader(h, buf, h->generation, h->epoch, h->capacity);
    err = rfsWriteAt(h, h->fd, 0, buf, RFS_HEADER_SIZE);

    if (err != ESP_OK)
    {
      //-- Header still carries the previous epoch; retry on next flush.
      h->epoch = previousEpoch;
      return err;
    }

    h->pendingEpochChange = false;
  }

  if (doSync)
  {
    err = rfsSyncFd(h, h->fd);
  }

  if (err == ESP_OK)
  {
    for (uint32_t p = 0; p < h->pendingCount; p++)
    {
      h->pending[p].used = false;
      h->pending[p].slotIndex = -1;
    }

    h->pendingCount = 0;
    h->dirty = false;
  }

  return err;
}

static esp_err_t rfsStage(ringFileHandle_t *h, const uint8_t *key, uint64_t seq,
                          uint32_t rev, const uint8_t *record)
{
  int32_t existing = rfsFindPending(h, key);

  if (existing >= 0 && h->pending[existing].slotIndex < 0)
  {
    //-- Coalesce repeated updates of the same record.
    h->pending[existing].seq = seq;
    h->pending[existing].rev = rev;
    memcpy(h->pendingRecords + (size_t)existing * h->recordSize, record,
           h->recordSize);
    h->dirty = true;
    return ESP_OK;
  }

  if (h->pendingCount >= h->maxPending)
  {
    esp_err_t err = rfsFlushPending(h, true);

    if (err != ESP_OK)
    {
      return err;
    }
  }

  //-- Entries are freed only by a successful flush, so the first unused
  //-- entry is the append position.
  uint32_t target = h->pendingCount;

  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (!h->pending[p].used)
    {
      target = p;
      break;
    }
  }

  if (target >= h->maxPending)
  {
    return ESP_ERR_NO_MEM;
  }

  h->pending[target].seq = seq;
  h->pending[target].rev = rev;
  h->pending[target].slotIndex = -1;
  h->pending[target].used = true;
  memcpy(h->pendingRecords + (size_t)target * h->recordSize, record, h->recordSize);

  if (target == h->pendingCount)
  {
    h->pendingCount++;
  }

  h->dirty = true;
  return ESP_OK;
}

//-- ---------------------------------------------------------------------------
//-- Header encode / decode
//-- ---------------------------------------------------------------------------

static void rfsEncodeHeader(const ringFileHandle_t *h, uint8_t *buf,
                            uint64_t generation, uint64_t epoch,
                            uint32_t capacity)
{
  memset(buf, 0xFF, RFS_HEADER_SIZE);
  rfsPutU32(buf, RFS_MAGIC);
  rfsPutU16(buf + 4, RFS_FORMAT_VERSION);
  rfsPutU16(buf + 6, RFS_HEADER_SIZE);
  rfsPutU64(buf + 8, generation);

  //-- schemaId string at [16,32): NUL-terminated, at most 12 characters;
  //-- bytes after the terminator stay 0xFF. Validated at create time.
  memcpy(buf + 16, h->schemaId, strlen(h->schemaId) + 1u);

  rfsPutU32(buf + 32, h->recordSize);
  rfsPutU32(buf + 36, capacity);
  rfsPutU32(buf + 40, h->granularity);
  rfsPutU32(buf + 44, 0);
  rfsPutU64(buf + 48, epoch);

  rfsPutU32(buf + 88, rfsCrc32(buf, 88));
}

static esp_err_t rfsDecodeHeader(const uint8_t *buf, int64_t fileLength,
                                 ringFileHandle_t *probe, bool *headerOk,
                                 ringFileMetaIssue_t *issue)
{
  *headerOk = false;
  *issue = RING_FILE_META_OK;

  if (fileLength < (int64_t)RFS_HEADER_SIZE)
  {
    *issue = RING_FILE_META_SHORT_FILE;
    return RING_FILE_STORE_ERR_CORRUPT;
  }

  if (rfsGetU32(buf) != RFS_MAGIC)
  {
    *issue = RING_FILE_META_BAD_MAGIC;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  uint16_t formatVersion = rfsGetU16(buf + 4);

  if (formatVersion < RFS_FORMAT_VERSION_MIN || formatVersion > RFS_FORMAT_VERSION)
  {
    *issue = RING_FILE_META_BAD_FORMAT_VERSION;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  if (rfsGetU16(buf + 6) != RFS_HEADER_SIZE)
  {
    *issue = RING_FILE_META_BAD_GEOMETRY;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  if (rfsGetU32(buf + 88) != rfsCrc32(buf, 88))
  {
    *issue = RING_FILE_META_BAD_CHECKSUM;
    return RING_FILE_STORE_ERR_CORRUPT;
  }

  uint32_t recordSize = rfsGetU32(buf + 32);
  uint32_t capacity = rfsGetU32(buf + 36);
  uint32_t granularity = rfsGetU32(buf + 40);
  uint32_t flags = rfsGetU32(buf + 44);

  if (recordSize <= RFS_KEY_FIELDS_SIZE || recordSize > RFS_MAX_RECORD_SIZE ||
      capacity == 0 || capacity > RFS_MAX_CAPACITY ||
      granularity < RING_FILE_GRANULARITY_YEAR ||
      granularity > RING_FILE_GRANULARITY_SECOND || flags != 0u)
  {
    *issue = RING_FILE_META_BAD_GEOMETRY;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  //-- schemaId string at [16,32): a NUL-terminated string of at most 12
  //-- characters; bytes after the terminator must be 0xFF. The string must
  //-- be one of the known file types and match the stored granularity.
  if (buf[16 + RFS_HEADER_NAME_SIZE - 1u] != 0xFFu)
  {
    *issue = RING_FILE_META_BAD_GEOMETRY;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  uint32_t nameEnd = 0;
  bool terminated = false;

  //-- The terminator sits at position 0..12 of the 16-byte field.
  for (uint32_t i = 0; i <= RING_FILE_SCHEMA_MAX_LEN; i++)
  {
    if (buf[16u + i] == 0u)
    {
      nameEnd = i;
      terminated = true;
      break;
    }
  }

  if (!terminated)
  {
    *issue = RING_FILE_META_BAD_GEOMETRY;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  char schemaId[RING_FILE_SCHEMA_MAX_LEN + 1u];
  memcpy(schemaId, buf + 16, nameEnd);
  schemaId[nameEnd] = '\0';

  for (uint32_t i = nameEnd + 1u; i < RFS_HEADER_NAME_SIZE; i++)
  {
    if (buf[16u + i] != 0xFFu)
    {
      *issue = RING_FILE_META_BAD_GEOMETRY;
      return RING_FILE_STORE_ERR_FORMAT;
    }
  }

  ringFileGranularity_t mapped = RING_FILE_GRANULARITY_NONE;

  if (!rfsTimeSchemaIdToGranularity(schemaId, &mapped) ||
      (uint32_t)mapped != granularity)
  {
    *issue = RING_FILE_META_BAD_GEOMETRY;
    return RING_FILE_STORE_ERR_FORMAT;
  }

  for (uint32_t i = 56; i < 88; i++)
  {
    if (buf[i] != 0xFFu)
    {
      *issue = RING_FILE_META_BAD_GEOMETRY;
      return RING_FILE_STORE_ERR_FORMAT;
    }
  }

  uint32_t slotSize = RFS_SLOT_HDR_SIZE + recordSize;
  uint64_t expectedMax =
      (uint64_t)RFS_HEADER_SIZE + (uint64_t)(capacity + 1u) * slotSize;

  if ((uint64_t)fileLength > expectedMax)
  {
    *issue = RING_FILE_META_BAD_GEOMETRY;
    return RING_FILE_STORE_ERR_CORRUPT;
  }

  memcpy(probe->schemaId, schemaId, nameEnd + 1u);
  probe->granularity = granularity;
  probe->payloadSize = recordSize - RFS_KEY_FIELDS_SIZE;
  probe->recordSize = recordSize;
  probe->capacity = capacity;
  probe->slotSize = slotSize;
  probe->generation = rfsGetU64(buf + 8);
  probe->epoch = rfsGetU64(buf + 48);
  *headerOk = true;
  return ESP_OK;
}

static bool rfsPathExists(const char *path)
{
  int fd = rfsIoOpen(path, O_RDONLY, 0);

  if (fd < 0)
  {
    return false;
  }

  rfsIoClose(fd);
  return true;
}

//-- Read and decode the header of an open candidate file.
static esp_err_t rfsProbeHeader(int fd, ringFileHandle_t *probe)
{
  uint8_t buf[RFS_HEADER_SIZE];
  bool eof = false;
  bool headerOk = false;
  ringFileMetaIssue_t issue = RING_FILE_META_OK;

  if (!rfsReadAt(fd, 0, buf, RFS_HEADER_SIZE, &eof))
  {
    return RING_FILE_STORE_ERR_CORRUPT;
  }

  int64_t fileLength = rfsFileLength(fd);

  if (fileLength < 0)
  {
    return RING_FILE_STORE_ERR_IO;
  }

  return rfsDecodeHeader(buf, fileLength, probe, &headerOk, &issue);
}

//-- ---------------------------------------------------------------------------
//-- Handle setup / teardown helpers
//-- ---------------------------------------------------------------------------

static esp_err_t rfsAcquireLock(ringFileHandle_t *h)
{
  int lfd = rfsIoOpen(h->lockPath, O_CREAT | O_EXCL | O_WRONLY, 0644);

  if (lfd < 0)
  {
    return (errno == EEXIST) ? RING_FILE_STORE_ERR_BUSY : rfsErrFromErrno();
  }

  h->lockFd = lfd;
  h->lockHeld = true;
  return ESP_OK;
}

static void rfsReleaseLock(ringFileHandle_t *h)
{
  if (h->lockHeld)
  {
    rfsIoClose(h->lockFd);
    rfsIoUnlink(h->lockPath);
    h->lockHeld = false;
  }
}

//-- Open the file, recovering a previous interrupted resize when possible.
//-- Assumes h->path and the derived paths are set and scratch is allocated.
static esp_err_t rfsOpenDataFile(ringFileHandle_t *h)
{
  for (int attempt = 0; attempt < 3; attempt++)
  {
    int fd = rfsIoOpen(h->path, h->readOnly ? O_RDONLY : O_RDWR, 0);

    if (fd < 0)
    {
      if (errno != ENOENT)
      {
        return rfsErrFromErrno();
      }

      //-- Missing file: a rename may have been interrupted.
      if (h->readOnly)
      {
        if (rfsPathExists(h->oldPath) || rfsPathExists(h->newPath))
        {
          return RING_FILE_STORE_ERR_RECOVERY_REQUIRED;
        }

        return ESP_ERR_NOT_FOUND;
      }

      bool recovered = false;

      if (rfsPathExists(h->oldPath))
      {
        if (rfsIoRename(h->oldPath, h->path) != 0)
        {
          return RING_FILE_STORE_ERR_IO;
        }

        recovered = true;
      }
      else if (rfsPathExists(h->newPath))
      {
        if (rfsIoRename(h->newPath, h->path) != 0)
        {
          return RING_FILE_STORE_ERR_IO;
        }

        recovered = true;
      }

      if (!recovered)
      {
        return ESP_ERR_NOT_FOUND;
      }

      continue;
    }

    uint8_t buf[RFS_HEADER_SIZE];
    bool eof = false;

    if (!rfsReadAt(fd, 0, buf, RFS_HEADER_SIZE, &eof))
    {
      rfsIoClose(fd);
      return RING_FILE_STORE_ERR_CORRUPT;
    }

    int64_t fileLength = rfsFileLength(fd);

    if (fileLength < 0)
    {
      rfsIoClose(fd);
      return RING_FILE_STORE_ERR_IO;
    }

    ringFileHandle_t probe;
    memset(&probe, 0, sizeof(probe));
    bool headerOk = false;
    ringFileMetaIssue_t issue = RING_FILE_META_OK;
    esp_err_t err = rfsDecodeHeader(buf, fileLength, &probe, &headerOk, &issue);

    if (err != ESP_OK)
    {
      if (!h->readOnly && rfsPathExists(h->oldPath))
      {
        //-- Recover an interrupted resize switch: discard the damaged
        //-- candidate and restore the previous file.
        rfsIoClose(fd);
        rfsIoUnlink(h->path);

        if (rfsIoRename(h->oldPath, h->path) != 0)
        {
          return RING_FILE_STORE_ERR_IO;
        }

        continue;
      }

      if (h->readOnly && (rfsPathExists(h->oldPath) || rfsPathExists(h->newPath)))
      {
        rfsIoClose(fd);
        return RING_FILE_STORE_ERR_RECOVERY_REQUIRED;
      }

      rfsIoClose(fd);
      return err;
    }

    h->fd = fd;
    h->generation = probe.generation;
    h->epoch = probe.epoch;
    memcpy(h->schemaId, probe.schemaId, sizeof(h->schemaId));
    h->granularity = probe.granularity;
    h->payloadSize = probe.payloadSize;
    h->recordSize = probe.recordSize;
    h->capacity = probe.capacity;
    h->slotSize = probe.slotSize;

    return ESP_OK;
  }

  return RING_FILE_STORE_ERR_CORRUPT;
}

static esp_err_t rfsInitHandle(ringFileHandle_t *h, const char *path,
                               uint32_t maxPending)
{
  memset(h, 0, sizeof(*h));
  h->fd = -1;
  h->lockFd = -1;
  h->maxPending = maxPending;
  h->path = rfsStrdup(path);
  h->lockPath = rfsStrdupWithSuffix(path, ".lck");
  h->oldPath = rfsStrdupWithSuffix(path, ".rfsold");
  h->newPath = rfsStrdupWithSuffix(path, ".rfsnew");

  if (h->path == NULL || h->lockPath == NULL || h->oldPath == NULL ||
      h->newPath == NULL)
  {
    rfsFreeHandleMembers(h);
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

//-- Recompute derived state after the geometry is known.
static esp_err_t rfsDeriveState(ringFileHandle_t *h)
{
  esp_err_t err = rfsAllocScratch(h);

  if (err != ESP_OK)
  {
    return err;
  }

  uint64_t maxSeq = 0;
  bool any = false;
  uint32_t slots = rfsSlotCount(h);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(h, idx, &hdr))
    {
      continue;
    }

    if (!any || hdr.seq > maxSeq)
    {
      maxSeq = hdr.seq;
      any = true;
    }
  }

  h->nextSeq = any ? (maxSeq + 1u) : h->epoch;

  if (h->nextSeq < h->epoch)
  {
    h->nextSeq = h->epoch;
  }

  h->count = rfsCountActive(h);
  h->nextSlotHint = 0;
  h->mutationStamp = 0;
  return ESP_OK;
}

//-- ---------------------------------------------------------------------------
//-- Configuration validation
//-- ---------------------------------------------------------------------------

static esp_err_t rfsValidateConfig(const ringFileConfig_t *config,
                                   uint32_t *outGranularity,
                                   uint32_t *outMaxPending)
{
  if (config->maxRecords == 0 || config->maxRecords > RFS_MAX_CAPACITY)
  {
    return ESP_ERR_INVALID_ARG;
  }

  ringFileGranularity_t granularity = RING_FILE_GRANULARITY_NONE;

  if (!rfsTimeSchemaIdToGranularity(config->schemaId, &granularity))
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (config->recordSize == 0 || config->recordSize > RFS_MAX_PAYLOAD_SIZE)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (config->persistMode != RING_FILE_PERSIST_IMMEDIATE &&
      config->persistMode != RING_FILE_PERSIST_BUFFERED)
  {
    return ESP_ERR_INVALID_ARG;
  }

  uint32_t maxPending = config->maxPendingRecords;

  if (config->persistMode == RING_FILE_PERSIST_IMMEDIATE)
  {
    maxPending = 1;
  }
  else if (maxPending == 0)
  {
    maxPending = RFS_DEFAULT_MAX_PENDING;
  }

  if (maxPending > RFS_MAX_PENDING_LIMIT)
  {
    return ESP_ERR_INVALID_ARG;
  }

  *outMaxPending = maxPending;
  *outGranularity = (uint32_t)granularity;

  uint64_t slotSize =
      RFS_SLOT_HDR_SIZE + RFS_KEY_FIELDS_SIZE + (uint64_t)config->recordSize;
  uint64_t storage =
      (uint64_t)RFS_HEADER_SIZE + (uint64_t)(config->maxRecords + 1u) * slotSize;

  if (storage < (uint64_t)RFS_HEADER_SIZE)
  {
    return ESP_ERR_INVALID_SIZE;
  }

  return ESP_OK;
}

static esp_err_t rfsValidateOpenConfig(const ringFileOpenConfig_t *openConfig,
                                       uint32_t *outMaxPending)
{
  if (openConfig->persistMode != RING_FILE_PERSIST_IMMEDIATE &&
      openConfig->persistMode != RING_FILE_PERSIST_BUFFERED)
  {
    return ESP_ERR_INVALID_ARG;
  }

  uint32_t maxPending = openConfig->maxPendingRecords;

  if (openConfig->persistMode == RING_FILE_PERSIST_IMMEDIATE)
  {
    maxPending = 1;
  }
  else if (maxPending == 0)
  {
    maxPending = RFS_DEFAULT_MAX_PENDING;
  }

  if (maxPending > RFS_MAX_PENDING_LIMIT)
  {
    return ESP_ERR_INVALID_ARG;
  }

  *outMaxPending = maxPending;
  return ESP_OK;
}

//-- ---------------------------------------------------------------------------
//-- Public API
//-- ---------------------------------------------------------------------------

//-- ---------------------------------------------------------------------------
//-- Boot notice
//--
//-- projectPrompt item 6: after the first successful create or open per boot,
//-- log the copyright/warranty notice with one ESP_LOGI() per line, using the
//-- tag ringFileStore.
//-- ---------------------------------------------------------------------------

static void rfsLogBootNotice(void)
{
  static bool sBootNoticeLogged = false;

  if (!sBootNoticeLogged)
  {
    sBootNoticeLogged = true;
    ESP_LOGI("ringFileStore", "ringFileStore  Copyright (C) 2026 Willem Aandewiel");
    ESP_LOGI("ringFileStore", "This program comes with ABSOLUTELY NO WARRANTY.");
    ESP_LOGI("ringFileStore", "This is free software, and you are welcome to redistribute it");
    ESP_LOGI("ringFileStore", "under the terms described in the LICENSE file.");
  }
}

esp_err_t ringFileCreate(const char *path, const ringFileConfig_t *config,
                         ringFileHandle_t **outHandle)
{
  if (path == NULL || config == NULL || outHandle == NULL || path[0] == '\0')
  {
    return ESP_ERR_INVALID_ARG;
  }

  uint32_t maxPending = 0;
  uint32_t granularity = 0;
  esp_err_t err = rfsValidateConfig(config, &granularity, &maxPending);

  if (err != ESP_OK)
  {
    return err;
  }

  ringFileHandle_t *h = (ringFileHandle_t *)calloc(1, sizeof(*h));

  if (h == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  err = rfsInitHandle(h, path, maxPending);

  if (err != ESP_OK)
  {
    free(h);
    return err;
  }

  strncpy(h->schemaId, config->schemaId, RING_FILE_SCHEMA_MAX_LEN);
  h->schemaId[RING_FILE_SCHEMA_MAX_LEN] = '\0';
  h->granularity = granularity;
  h->payloadSize = config->recordSize;
  h->recordSize = RFS_KEY_FIELDS_SIZE + config->recordSize;
  h->capacity = config->maxRecords;
  h->slotSize = RFS_SLOT_HDR_SIZE + h->recordSize;
  h->epoch = 0;
  h->generation = 0;
  h->ownsLock = true;
  h->lock = xSemaphoreCreateMutex();

  if (h->lock == NULL)
  {
    rfsFreeHandleMembers(h);
    free(h);
    return ESP_ERR_NO_MEM;
  }

  err = rfsAllocScratch(h);

  if (err != ESP_OK)
  {
    rfsFreeHandleMembers(h);
    free(h);
    return err;
  }

  h->pending = (rfsPendingEntry_t *)calloc(maxPending, sizeof(rfsPendingEntry_t));
  h->pendingRecords = (uint8_t *)malloc((size_t)maxPending * h->recordSize);

  if (h->pending == NULL || h->pendingRecords == NULL)
  {
    rfsFreeHandleMembers(h);
    free(h);
    return ESP_ERR_NO_MEM;
  }

  int fd = rfsIoOpen(path, O_RDWR | O_CREAT | O_EXCL, 0644);

  if (fd < 0)
  {
    esp_err_t mapErr = (errno == EEXIST) ? RING_FILE_STORE_ERR_ALREADY_EXISTS
                                         : rfsErrFromErrno();
    rfsFreeHandleMembers(h);
    free(h);
    return mapErr;
  }

  h->fd = fd;
  err = rfsAcquireLock(h);

  if (err != ESP_OK)
  {
    rfsIoClose(fd);
    h->fd = -1;
    rfsIoUnlink(path);
    rfsFreeHandleMembers(h);
    free(h);
    return err;
  }

  uint8_t buf[RFS_HEADER_SIZE];
  rfsEncodeHeader(h, buf, 0, 0, h->capacity);
  err = rfsWriteAt(h, h->fd, 0, buf, RFS_HEADER_SIZE);

  if (err == ESP_OK)
  {
    err = rfsSyncFd(h, h->fd);
  }

  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "create commit failed: %s", esp_err_to_name(err));
    rfsIoClose(h->fd);
    h->fd = -1;
    rfsReleaseLock(h);
    rfsIoUnlink(path);
    rfsFreeHandleMembers(h);
    free(h);
    return err;
  }

  h->nextSeq = 0;
  h->count = 0;
  rfsLogBootNotice();
  *outHandle = h;
  return ESP_OK;
}

esp_err_t ringFileOpen(const char *path, const ringFileOpenConfig_t *openConfig,
                       ringFileHandle_t **outHandle)
{
  if (path == NULL || openConfig == NULL || outHandle == NULL || path[0] == '\0')
  {
    return ESP_ERR_INVALID_ARG;
  }

  uint32_t maxPending = 0;
  esp_err_t err = rfsValidateOpenConfig(openConfig, &maxPending);

  if (err != ESP_OK)
  {
    return err;
  }

  ringFileHandle_t *h = (ringFileHandle_t *)calloc(1, sizeof(*h));

  if (h == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  err = rfsInitHandle(h, path, maxPending);

  if (err != ESP_OK)
  {
    free(h);
    return err;
  }

  h->readOnly = openConfig->readOnly;

  if (!h->readOnly)
  {
    err = rfsAcquireLock(h);

    if (err != ESP_OK)
    {
      rfsFreeHandleMembers(h);
      free(h);
      return err;
    }
  }

  err = rfsOpenDataFile(h);

  if (err != ESP_OK)
  {
    rfsReleaseLock(h);
    rfsFreeHandleMembers(h);
    free(h);
    return err;
  }

  if (openConfig->expectedSchemaId == NULL ||
      strcmp(h->schemaId, openConfig->expectedSchemaId) != 0 ||
      h->payloadSize != openConfig->expectedRecordSize)
  {
    rfsIoClose(h->fd);
    h->fd = -1;
    rfsReleaseLock(h);
    rfsFreeHandleMembers(h);
    free(h);
    return RING_FILE_STORE_ERR_SCHEMA_MISMATCH;
  }

  err = rfsDeriveState(h);

  if (err != ESP_OK)
  {
    rfsIoClose(h->fd);
    h->fd = -1;
    rfsReleaseLock(h);
    rfsFreeHandleMembers(h);
    free(h);
    return err;
  }

  h->pending = (rfsPendingEntry_t *)calloc(maxPending, sizeof(rfsPendingEntry_t));
  h->pendingRecords = (uint8_t *)malloc((size_t)maxPending * h->recordSize);

  if (h->pending == NULL || h->pendingRecords == NULL)
  {
    rfsIoClose(h->fd);
    h->fd = -1;
    rfsReleaseLock(h);
    rfsFreeHandleMembers(h);
    free(h);
    return ESP_ERR_NO_MEM;
  }

  //-- Remove resize leftovers once the main file is known good.
  if (!h->readOnly)
  {
    if (rfsPathExists(h->oldPath))
    {
      rfsIoUnlink(h->oldPath);
    }

    if (rfsPathExists(h->newPath))
    {
      rfsIoUnlink(h->newPath);
    }
  }

  h->ownsLock = true;
  h->lock = xSemaphoreCreateMutex();

  if (h->lock == NULL)
  {
    rfsIoClose(h->fd);
    h->fd = -1;
    rfsReleaseLock(h);
    rfsFreeHandleMembers(h);
    free(h);
    return ESP_ERR_NO_MEM;
  }

  rfsLogBootNotice();
  *outHandle = h;
  return ESP_OK;
}

void ringFileClose(ringFileHandle_t *handle)
{
  if (handle == NULL)
  {
    return;
  }

  if (handle->lock != NULL)
  {
    RFS_ENTER(handle);
  }

  esp_err_t err = ESP_OK;

  if (!handle->readOnly && handle->fd >= 0 && !handle->ioFaulted)
  {
    err = rfsFlushPending(handle, true);

    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "close commit failed: %s", esp_err_to_name(err));
    }
  }

  if (handle->fd >= 0)
  {
    if (rfsIoClose(handle->fd) != 0)
    {
      ESP_LOGW(TAG, "close(%s) failed: %d", handle->path, errno);
    }

    handle->fd = -1;
  }

  if (handle->lock != NULL)
  {
    RFS_EXIT(handle);
  }

  rfsReleaseLock(handle);
  rfsFreeHandleMembers(handle);
  free(handle);
}

static esp_err_t rfsCheckWritable(ringFileHandle_t *h)
{
  if (h->readOnly)
  {
    return RING_FILE_STORE_ERR_READONLY;
  }

  if (h->ioFaulted)
  {
    return RING_FILE_STORE_ERR_IO_FAULTED;
  }

  return ESP_OK;
}

//-- ---------------------------------------------------------------------------
//-- Timestamp write helpers (ringFileAdd / ringFileUpsert / ringFileEdit)
//-- ---------------------------------------------------------------------------

//-- Newest stored period among committed members (disk and staged).
//-- Returns false when the ring holds no committed member.
static bool rfsMaxOrderLocked(ringFileHandle_t *h, uint64_t *outOrder)
{
  bool found = false;
  *outOrder = 0;
  uint32_t slots = rfsSlotCount(h);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(h, idx, &hdr))
    {
      continue;
    }

    //-- Derive key and period from the slot image before rfsBestCopy()
    //-- reuses the scratch buffers.
    rfsExtractKey(h, h->slotBuf, h->keyBufB);
    uint64_t order = rfsOrderOfRecord(h, h->slotBuf);

    rfsMember_t cand;
    rfsBestCopy(h, h->keyBufB, &cand);

    if (!cand.found || cand.pending || (uint32_t)cand.slot != idx)
    {
      continue;
    }

    if (rfsRankGreater(h, order) >= h->capacity)
    {
      continue;
    }

    if (!found || order > *outOrder)
    {
      found = true;
      *outOrder = order;
    }
  }

  for (uint32_t p = 0; p < h->pendingCount; p++)
  {
    if (!h->pending[p].used)
    {
      continue;
    }

    uint64_t order = rfsOrderOfPending(h, p);

    if (rfsRankGreater(h, order) >= h->capacity)
    {
      continue;
    }

    if (!found || order > *outOrder)
    {
      found = true;
      *outOrder = order;
    }
  }

  return found;
}

//-- Shared append path of ringFileAdd and ringFileUpsert: overwrite the
//-- active record of the same bucket (many timestamps map to one bucket),
//-- otherwise append only when the period is newer than the newest stored
//-- period. key is the 8-byte usedKey, order its period.
static esp_err_t rfsTimeAddLocked(ringFileHandle_t *handle, const uint8_t *key,
                                  uint64_t order, const uint8_t *record)
{
  rfsMember_t member;

  if (rfsFindActive(handle, key, &member))
  {
    return rfsUpdateLocked(handle, key, record);
  }

  uint64_t newestOrder = 0;

  if (rfsMaxOrderLocked(handle, &newestOrder) && order <= newestOrder)
  {
    //-- Time always moves forward: reject a clock rewind or a gap fill.
    return RING_FILE_STORE_ERR_NOT_NEWER;
  }

  uint64_t seq = handle->nextSeq++;
  esp_err_t err = rfsStage(handle, key, seq, 0, record);

  if (err == ESP_OK)
  {
    handle->count = (handle->count < handle->capacity) ? handle->count + 1u
                                                       : handle->capacity;
    handle->mutationStamp++;

    if (handle->maxPending == 1)
    {
      err = rfsFlushPending(handle, true);
    }
  }

  return err;
}

esp_err_t ringFileAdd(ringFileHandle_t *handle, uint64_t timeStamp,
                      const void *record)
{
  if (handle == NULL || record == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsCheckWritable(handle);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  uint64_t order = 0;
  err = rfsBuildRecordLocked(handle, timeStamp, record, handle->recBuf, &order);

  if (err == ESP_OK)
  {
    uint8_t key[8];
    rfsPutU64(key, rfsKeyOfRecord(handle->recBuf));
    err = rfsTimeAddLocked(handle, key, order, handle->recBuf);
  }

  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileReadByKey(ringFileHandle_t *handle, uint64_t usedKey,
                            void *outRecord)
{
  if (handle == NULL || outRecord == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);

  uint8_t key[8];
  rfsPutU64(key, usedKey);

  rfsMember_t member;

  if (!rfsFindActive(handle, key, &member))
  {
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_KEY_NOT_FOUND;
  }

  esp_err_t err = rfsMemberLoad(handle, &member, (uint8_t *)outRecord);
  RFS_EXIT(handle);
  return err;
}

static esp_err_t rfsUpdateLocked(ringFileHandle_t *handle, const uint8_t *key,
                                 const uint8_t *record)
{
  if (rfsGetU64(key) != rfsKeyOfRecord(record))
  {
    return ESP_ERR_INVALID_ARG;
  }

  rfsMember_t member;

  if (!rfsFindActive(handle, key, &member))
  {
    return RING_FILE_STORE_ERR_KEY_NOT_FOUND;
  }

  uint8_t current[RFS_MAX_RECORD_SIZE];
  esp_err_t err = rfsMemberLoad(handle, &member, current);

  if (err != ESP_OK)
  {
    return err;
  }

  if (memcmp(current, record, handle->recordSize) == 0)
  {
    //-- Unchanged replacement: never a write.
    return ESP_OK;
  }

  err = rfsStage(handle, key, member.seq, member.rev + 1u, record);

  if (err == ESP_OK)
  {
    handle->mutationStamp++;

    if (handle->maxPending == 1)
    {
      err = rfsFlushPending(handle, true);
    }
  }

  return err;
}

esp_err_t ringFileUpdateByKey(ringFileHandle_t *handle, uint64_t usedKey,
                              uint64_t timeStamp, const void *record)
{
  if (handle == NULL || record == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsCheckWritable(handle);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  err = rfsBuildRecordLocked(handle, timeStamp, record, handle->recBuf, NULL);

  if (err == ESP_OK)
  {
    if (rfsKeyOfRecord(handle->recBuf) != usedKey)
    {
      //-- The usedKey derived from timeStamp must match the lookup key.
      err = ESP_ERR_INVALID_ARG;
    }
    else
    {
      uint8_t key[8];
      rfsPutU64(key, usedKey);
      err = rfsUpdateLocked(handle, key, handle->recBuf);
    }
  }

  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileUpsert(ringFileHandle_t *handle, uint64_t timeStamp,
                         const void *record)
{
  if (handle == NULL || record == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsCheckWritable(handle);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  uint64_t order = 0;
  err = rfsBuildRecordLocked(handle, timeStamp, record, handle->recBuf, &order);

  if (err == ESP_OK)
  {
    uint8_t key[8];
    rfsPutU64(key, rfsKeyOfRecord(handle->recBuf));
    err = rfsTimeAddLocked(handle, key, order, handle->recBuf);
  }

  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileEdit(ringFileHandle_t *handle, uint64_t timeStamp,
                       const void *record)
{
  if (handle == NULL || record == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsCheckWritable(handle);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  uint64_t order = 0;
  err = rfsBuildRecordLocked(handle, timeStamp, record, handle->recBuf, &order);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  uint8_t key[8];
  rfsPutU64(key, rfsKeyOfRecord(handle->recBuf));

  rfsMember_t member;

  if (rfsFindActive(handle, key, &member))
  {
    //-- Backfill an active bucket with any timestamp.
    err = rfsUpdateLocked(handle, key, handle->recBuf);
    RFS_EXIT(handle);
    return err;
  }

  uint64_t newestOrder = 0;

  if (rfsMaxOrderLocked(handle, &newestOrder))
  {
    //-- Insertable only inside the retained window; older buckets were
    //-- evicted long ago. An empty ring accepts any bucket.
    uint64_t windowLow = (newestOrder + 1u > handle->capacity)
                             ? (newestOrder - handle->capacity + 1u)
                             : 0u;

    if (order < windowLow)
    {
      RFS_EXIT(handle);
      return RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE;
    }
  }

  uint64_t seq = handle->nextSeq++;
  err = rfsStage(handle, key, seq, 0, handle->recBuf);

  if (err == ESP_OK)
  {
    handle->count = (handle->count < handle->capacity) ? handle->count + 1u
                                                       : handle->capacity;
    handle->mutationStamp++;

    if (handle->maxPending == 1)
    {
      err = rfsFlushPending(handle, true);
    }
  }

  RFS_EXIT(handle);
  return err;
}

static esp_err_t rfsReadEndLocked(ringFileHandle_t *handle, void *outRecord,
                                  bool oldest)
{
  bool found = false;
  rfsMember_t best;
  uint64_t bestOrder = 0;

  memset(&best, 0, sizeof(best));

  uint32_t slots = rfsSlotCount(handle);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(handle, idx, &hdr))
    {
      continue;
    }

    rfsExtractKey(handle, handle->slotBuf, handle->keyBufB);
    uint64_t order = rfsOrderOfRecord(handle, handle->slotBuf);

    rfsMember_t cand;
    rfsBestCopy(handle, handle->keyBufB, &cand);

    if (!cand.found || cand.pending || (uint32_t)cand.slot != idx)
    {
      continue;
    }

    if (rfsRankGreater(handle, order) >= handle->capacity)
    {
      continue;
    }

    if (!found || (oldest ? order < bestOrder : order > bestOrder))
    {
      found = true;
      best = cand;
      bestOrder = order;
    }
  }

  for (uint32_t p = 0; p < handle->pendingCount; p++)
  {
    if (!handle->pending[p].used)
    {
      continue;
    }

    uint64_t order = rfsOrderOfPending(handle, p);

    if (rfsRankGreater(handle, order) >= handle->capacity)
    {
      continue;
    }

    rfsMember_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.found = true;
    cand.seq = handle->pending[p].seq;
    cand.rev = handle->pending[p].rev;
    cand.pending = true;
    cand.pendingIndex = p;
    cand.order = order;

    if (!found || (oldest ? order < bestOrder : order > bestOrder))
    {
      found = true;
      best = cand;
      bestOrder = order;
    }
  }

  if (!found)
  {
    return RING_FILE_STORE_ERR_RING_EMPTY;
  }

  return rfsMemberLoad(handle, &best, (uint8_t *)outRecord);
}

esp_err_t ringFileReadOldest(ringFileHandle_t *handle, void *outRecord)
{
  if (handle == NULL || outRecord == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsReadEndLocked(handle, outRecord, true);
  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileReadNewest(ringFileHandle_t *handle, void *outRecord)
{
  if (handle == NULL || outRecord == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsReadEndLocked(handle, outRecord, false);
  RFS_EXIT(handle);
  return err;
}

//-- Locate the committed member with the smallest period greater than (or,
//-- when inclusive, equal to) 'cursor' in forward direction, or the largest
//-- period smaller than 'cursor' backwards. Distinct keys have distinct
//-- periods, so the cursor needs no tie-breaker.
static bool rfsFindNeighbor(ringFileHandle_t *handle, uint64_t cursor, bool dirNext,
                            bool inclusive, rfsMember_t *out)
{
  bool found = false;
  uint64_t outOrder = 0;
  uint32_t slots = rfsSlotCount(handle);

  for (uint32_t idx = 0; idx < slots; idx++)
  {
    rfsSlotHdr_t hdr;

    if (!rfsSlotHeaderValid(handle, idx, &hdr))
    {
      continue;
    }

    rfsExtractKey(handle, handle->slotBuf, handle->keyBufB);
    uint64_t order = rfsOrderOfRecord(handle, handle->slotBuf);

    bool passes;

    if (dirNext)
    {
      passes = inclusive ? (order >= cursor) : (order > cursor);
    }
    else
    {
      passes = (order < cursor);
    }

    if (!passes)
    {
      continue;
    }

    rfsMember_t cand;
    rfsBestCopy(handle, handle->keyBufB, &cand);

    if (!cand.found || cand.pending || (uint32_t)cand.slot != idx)
    {
      continue;
    }

    if (rfsRankGreater(handle, order) >= handle->capacity)
    {
      continue;
    }

    if (!found || (dirNext ? order < outOrder : order > outOrder))
    {
      found = true;
      *out = cand;
      outOrder = order;
    }
  }

  for (uint32_t p = 0; p < handle->pendingCount; p++)
  {
    if (!handle->pending[p].used)
    {
      continue;
    }

    uint64_t pseq = handle->pending[p].seq;
    uint64_t order = rfsOrderOfPending(handle, p);
    bool passes;

    if (dirNext)
    {
      passes = inclusive ? (order >= cursor) : (order > cursor);
    }
    else
    {
      passes = (order < cursor);
    }

    if (!passes)
    {
      continue;
    }

    if (rfsRankGreater(handle, order) >= handle->capacity)
    {
      continue;
    }

    rfsMember_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.found = true;
    cand.seq = pseq;
    cand.rev = handle->pending[p].rev;
    cand.pending = true;
    cand.pendingIndex = p;
    cand.order = order;

    if (!found || (dirNext ? order < outOrder : order > outOrder))
    {
      found = true;
      *out = cand;
      outOrder = order;
    }
  }

  return found;
}

esp_err_t ringFileIteratorBegin(ringFileHandle_t *handle,
                                ringFileIterator_t *iterator,
                                ringFileIterStart_t start,
                                const uint64_t *usedKey, void *outRecord)
{
  if (handle == NULL || iterator == NULL || outRecord == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  if (start == RING_FILE_ITER_KEY && usedKey == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  rfsMember_t member;
  memset(&member, 0, sizeof(member));
  esp_err_t err = ESP_OK;

  if (start == RING_FILE_ITER_KEY)
  {
    uint8_t key[8];
    rfsPutU64(key, *usedKey);

    if (!rfsFindActive(handle, key, &member))
    {
      RFS_EXIT(handle);
      return RING_FILE_STORE_ERR_KEY_NOT_FOUND;
    }
  }
  else
  {
    if (handle->count == 0)
    {
      RFS_EXIT(handle);
      return RING_FILE_STORE_ERR_RING_EMPTY;
    }

    bool oldest = (start == RING_FILE_ITER_OLDEST);

    if (!rfsFindNeighbor(handle, oldest ? 0 : UINT64_MAX, oldest, oldest, &member))
    {
      RFS_EXIT(handle);
      return RING_FILE_STORE_ERR_RING_EMPTY;
    }
  }

  err = rfsMemberLoad(handle, &member, (uint8_t *)outRecord);

  if (err == ESP_OK)
  {
    //-- The cursor holds the period of the last returned record.
    iterator->cursorOrder = member.order;
    iterator->stamp = handle->mutationStamp;
    iterator->slot = rfsSlotReportOf(handle, &member);
    iterator->begun = true;
  }
  else
  {
    iterator->begun = false;
  }

  RFS_EXIT(handle);
  return err;
}

static esp_err_t rfsIteratorStep(ringFileHandle_t *handle,
                                 ringFileIterator_t *iterator, void *outRecord,
                                 bool dirNext)
{
  RFS_ENTER(handle);

  if (!iterator->begun || iterator->stamp != handle->mutationStamp)
  {
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_ITER_INVALIDATED;
  }

  rfsMember_t member;
  memset(&member, 0, sizeof(member));

  if (!rfsFindNeighbor(handle, iterator->cursorOrder, dirNext, false, &member))
  {
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_ITER_END;
  }

  esp_err_t err = rfsMemberLoad(handle, &member, (uint8_t *)outRecord);

  if (err == ESP_OK)
  {
    iterator->cursorOrder = member.order;
    iterator->slot = rfsSlotReportOf(handle, &member);
  }

  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileIteratorNext(ringFileHandle_t *handle,
                               ringFileIterator_t *iterator, void *outRecord)
{
  if (handle == NULL || iterator == NULL || outRecord == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  return rfsIteratorStep(handle, iterator, outRecord, true);
}

esp_err_t ringFileIteratorPrevious(ringFileHandle_t *handle,
                                   ringFileIterator_t *iterator, void *outRecord)
{
  if (handle == NULL || iterator == NULL || outRecord == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  return rfsIteratorStep(handle, iterator, outRecord, false);
}

esp_err_t ringFileIteratorGetSlot(ringFileHandle_t *handle,
                                  const ringFileIterator_t *iterator,
                                  uint32_t *outSlot)
{
  if (handle == NULL || iterator == NULL || outSlot == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);

  if (!iterator->begun || iterator->stamp != handle->mutationStamp)
  {
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_ITER_INVALIDATED;
  }

  *outSlot = iterator->slot;
  RFS_EXIT(handle);
  return ESP_OK;
}

esp_err_t ringFileGetInfo(ringFileHandle_t *handle, ringFileInfo_t *outInfo)
{
  if (handle == NULL || outInfo == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  outInfo->count = handle->count;
  outInfo->capacity = handle->capacity;
  outInfo->payloadSize = handle->payloadSize;
  outInfo->recordSize = handle->recordSize;
  outInfo->storageSize =
      (uint64_t)RFS_HEADER_SIZE + (uint64_t)rfsSlotCount(handle) * handle->slotSize;
  outInfo->pendingCount = handle->pendingCount;
  outInfo->dirty = handle->dirty || handle->pendingEpochChange;
  memcpy(outInfo->schemaId, handle->schemaId, sizeof(outInfo->schemaId));
  outInfo->granularity = (ringFileGranularity_t)handle->granularity;
  RFS_EXIT(handle);
  return ESP_OK;
}

esp_err_t ringFileSync(ringFileHandle_t *handle)
{
  if (handle == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = ESP_OK;

  if (handle->readOnly)
  {
    err = (handle->pendingCount == 0) ? ESP_OK : RING_FILE_STORE_ERR_READONLY;
  }
  else if (handle->ioFaulted)
  {
    err = RING_FILE_STORE_ERR_IO_FAULTED;
  }
  else
  {
    err = rfsFlushPending(handle, true);
  }

  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileClear(ringFileHandle_t *handle)
{
  if (handle == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsCheckWritable(handle);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  //-- Drop staged data and hide every record with the header epoch.
  for (uint32_t p = 0; p < handle->pendingCount; p++)
  {
    handle->pending[p].used = false;
    handle->pending[p].slotIndex = -1;
  }

  handle->pendingCount = 0;
  handle->pendingEpochChange = true;
  handle->stagedEpoch = handle->nextSeq;
  handle->count = 0;
  handle->dirty = true;
  handle->mutationStamp++;

  if (handle->maxPending == 1)
  {
    err = rfsFlushPending(handle, true);
  }

  RFS_EXIT(handle);
  return err;
}

//-- k-th oldest committed member (0 = oldest). Streaming selection with
//-- bounded RAM: each step rescans for the smallest period above the
//-- previous.
static bool rfsFindKthOldest(ringFileHandle_t *h, uint32_t k, rfsMember_t *out)
{
  bool havePrev = false;
  uint64_t prev = 0;

  for (uint32_t i = 0; i <= k; i++)
  {
    bool found = false;
    uint64_t bestOrder = 0;
    rfsMember_t best;
    memset(&best, 0, sizeof(best));
    uint32_t slots = rfsSlotCount(h);

    for (uint32_t idx = 0; idx < slots; idx++)
    {
      rfsSlotHdr_t hdr;

      if (!rfsSlotHeaderValid(h, idx, &hdr))
      {
        continue;
      }

      rfsExtractKey(h, h->slotBuf, h->keyBufB);
      uint64_t order = rfsOrderOfRecord(h, h->slotBuf);

      if (havePrev && order <= prev)
      {
        continue;
      }

      rfsMember_t cand;
      rfsBestCopy(h, h->keyBufB, &cand);

      if (!cand.found || cand.pending || (uint32_t)cand.slot != idx)
      {
        continue;
      }

      if (rfsRankGreater(h, order) >= h->capacity)
      {
        continue;
      }

      if (!found || order < bestOrder)
      {
        found = true;
        best = cand;
        bestOrder = order;
      }
    }

    for (uint32_t p = 0; p < h->pendingCount; p++)
    {
      if (!h->pending[p].used)
      {
        continue;
      }

      uint64_t pseq = h->pending[p].seq;
      uint64_t order = rfsOrderOfPending(h, p);

      if (havePrev && order <= prev)
      {
        continue;
      }

      if (rfsRankGreater(h, order) >= h->capacity)
      {
        continue;
      }

      rfsMember_t cand;
      memset(&cand, 0, sizeof(cand));
      cand.found = true;
      cand.seq = pseq;
      cand.rev = h->pending[p].rev;
      cand.pending = true;
      cand.pendingIndex = p;
      cand.order = order;

      if (!found || order < bestOrder)
      {
        found = true;
        best = cand;
        bestOrder = order;
      }
    }

    if (!found)
    {
      return false;
    }

    *out = best;
    prev = bestOrder;
    havePrev = true;
  }

  return true;
}

//-- k-th record to stream into the resize candidate. The candidate holds
//-- the newest 'retained' members in bucket (period) order, so the first
//-- (count - retained) oldest members are skipped on a shrink.
static bool rfsFindRetainedMember(ringFileHandle_t *h, uint32_t k,
                                  uint32_t retained, rfsMember_t *out)
{
  uint32_t start = (h->count > retained) ? (h->count - retained) : 0;
  return rfsFindKthOldest(h, start + k, out);
}

esp_err_t ringFileResize(ringFileHandle_t *handle, uint32_t newCapacity)
{
  if (handle == NULL || newCapacity == 0 || newCapacity > RFS_MAX_CAPACITY)
  {
    return ESP_ERR_INVALID_ARG;
  }

  RFS_ENTER(handle);
  esp_err_t err = rfsCheckWritable(handle);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  err = rfsFlushPending(handle, true);

  if (err != ESP_OK)
  {
    RFS_EXIT(handle);
    return err;
  }

  if (newCapacity == handle->capacity)
  {
    RFS_EXIT(handle);
    return ESP_OK;
  }

  uint32_t slotSize = RFS_SLOT_HDR_SIZE + handle->recordSize;
  uint64_t storage =
      (uint64_t)RFS_HEADER_SIZE + (uint64_t)(newCapacity + 1u) * slotSize;

  if (storage < (uint64_t)RFS_HEADER_SIZE)
  {
    RFS_EXIT(handle);
    return ESP_ERR_INVALID_SIZE;
  }

  //-- Build the candidate on the same volume. The original file stays open
  //-- for streaming reads until the verified switch.
  rfsIoUnlink(handle->newPath);
  int nfd = rfsIoOpen(handle->newPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

  if (nfd < 0)
  {
    //-- The committed file is untouched; reopen it and keep using the
    //-- handle unchanged.
    esp_err_t mapErr = (errno == ENOENT) ? RING_FILE_STORE_ERR_IO
                                         : rfsErrFromErrno();
    rfsOpenDataFile(handle);
    RFS_EXIT(handle);
    return mapErr;
  }

  uint8_t buf[RFS_HEADER_SIZE];
  rfsEncodeHeader(handle, buf, handle->generation + 1u, handle->epoch,
                  newCapacity);
  err = rfsWriteAt(handle, nfd, 0, buf, RFS_HEADER_SIZE);

  uint32_t retained = (handle->count < newCapacity) ? handle->count : newCapacity;

  for (uint32_t k = 0; k < retained && err == ESP_OK; k++)
  {
    rfsMember_t member;
    memset(&member, 0, sizeof(member));

    //-- Stream the k-th oldest committed member (insertion order). On a
    //-- shrink, k runs over only the newest 'retained' members: the k-th
    //-- oldest of those is counted from the newest side.
    if (!rfsFindRetainedMember(handle, k, retained, &member))
    {
      err = ESP_ERR_INVALID_STATE;
      break;
    }

    uint8_t record[RFS_MAX_RECORD_SIZE];
    err = rfsMemberLoad(handle, &member, record);

    if (err != ESP_OK)
    {
      break;
    }

    uint8_t raw[RFS_SLOT_HDR_SIZE];
    rfsPutU32(raw, RFS_SLOT_MAGIC);
    rfsPutU64(raw + 4, member.seq);
    rfsPutU32(raw + 12, member.rev);
    rfsPutU32(raw + 16, rfsCrc32Two(raw + 4, 12, record, handle->recordSize));
    memcpy(handle->slotBuf, raw, RFS_SLOT_HDR_SIZE);
    memcpy(handle->slotBuf + RFS_SLOT_HDR_SIZE, record, handle->recordSize);
    err = rfsWriteAt(handle, nfd,
                     (uint64_t)RFS_HEADER_SIZE + (uint64_t)k * handle->slotSize,
                     handle->slotBuf, handle->slotSize);
  }

  if (err == ESP_OK)
  {
    if (rfsIoFsync(nfd) != 0)
    {
      err = RING_FILE_STORE_ERR_IO;
    }
  }

  if (rfsIoClose(nfd) != 0 && err == ESP_OK)
  {
    err = RING_FILE_STORE_ERR_IO;
  }

  if (err != ESP_OK)
  {
    //-- The committed file was never touched.
    handle->ioFaulted = true;
    rfsIoUnlink(handle->newPath);
    RFS_EXIT(handle);
    return err;
  }

  //-- Verify the candidate before switching.
  int vfd = rfsIoOpen(handle->newPath, O_RDONLY, 0);
  bool candidateOk = false;

  if (vfd >= 0)
  {
    ringFileHandle_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.fd = vfd;

    if (rfsProbeHeader(vfd, &probe) == ESP_OK && probe.capacity == newCapacity &&
        probe.recordSize == handle->recordSize &&
        probe.payloadSize == handle->payloadSize &&
        probe.granularity == handle->granularity &&
        strcmp(probe.schemaId, handle->schemaId) == 0)
    {
      candidateOk = true;
    }

    rfsIoClose(vfd);
  }

  if (!candidateOk)
  {
    //-- The candidate is broken but the committed file is untouched.
    rfsIoUnlink(handle->newPath);
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_CORRUPT;
  }

  //-- Close the original before the atomic switch.
  if (rfsIoClose(handle->fd) != 0)
  {
    handle->ioFaulted = true;
    handle->fd = -1;
    rfsIoUnlink(handle->newPath);
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_IO;
  }

  handle->fd = -1;

  //-- Atomic switch with enough redundancy to recover after interruption
  //-- between the two renames.
  if (rfsIoRename(handle->path, handle->oldPath) != 0)
  {
    rfsIoUnlink(handle->newPath);
    rfsOpenDataFile(handle);
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_IO;
  }

  if (rfsIoRename(handle->newPath, handle->path) != 0)
  {
    //-- Roll back so the committed ring stays reachable.
    if (rfsIoRename(handle->oldPath, handle->path) != 0)
    {
      ESP_LOGE(TAG, "rollback of interrupted resize failed; recovery needed");
      rfsOpenDataFile(handle);
      handle->ioFaulted = true;
      RFS_EXIT(handle);
      return RING_FILE_STORE_ERR_IO;
    }

    rfsIoUnlink(handle->newPath);
    rfsOpenDataFile(handle);
    RFS_EXIT(handle);
    return RING_FILE_STORE_ERR_IO;
  }

  rfsIoUnlink(handle->oldPath);

  //-- Reopen the resized file in place.
  err = rfsOpenDataFile(handle);

  if (err != ESP_OK)
  {
    handle->ioFaulted = true;
    RFS_EXIT(handle);
    return err;
  }

  err = rfsDeriveState(handle);
  handle->mutationStamp++;

  if (err != ESP_OK)
  {
    handle->ioFaulted = true;
  }

  RFS_EXIT(handle);
  return err;
}

esp_err_t ringFileValidate(const char *path, ringFileValidation_t *outValidation)
{
  if (path == NULL || outValidation == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  memset(outValidation, 0, sizeof(*outValidation));

  int fd = rfsIoOpen(path, O_RDONLY, 0);

  if (fd < 0)
  {
    return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : rfsErrFromErrno();
  }

  uint8_t buf[RFS_HEADER_SIZE];
  bool eof = false;

  if (!rfsReadAt(fd, 0, buf, RFS_HEADER_SIZE, &eof))
  {
    rfsIoClose(fd);
    outValidation->metadataIssue = RING_FILE_META_SHORT_FILE;
    return ESP_OK;
  }

  int64_t fileLength = rfsFileLength(fd);

  if (fileLength < 0)
  {
    rfsIoClose(fd);
    return RING_FILE_STORE_ERR_IO;
  }

  ringFileHandle_t probe;
  memset(&probe, 0, sizeof(probe));
  probe.fd = fd;
  bool headerOk = false;
  ringFileMetaIssue_t issue = RING_FILE_META_OK;
  rfsDecodeHeader(buf, fileLength, &probe, &headerOk, &issue);
  outValidation->metadataIssue = issue;
  outValidation->headerValid = headerOk;

  if (headerOk)
  {
    outValidation->capacity = probe.capacity;
    outValidation->recordSize = probe.recordSize;
    outValidation->slotCount = rfsSlotCount(&probe);

    esp_err_t scratchErr = rfsAllocScratch(&probe);

    if (scratchErr != ESP_OK)
    {
      rfsIoClose(fd);
      return scratchErr;
    }

    uint32_t slots = rfsSlotCount(&probe);

    for (uint32_t idx = 0; idx < slots; idx++)
    {
      rfsSlotHdr_t hdr;
      bool bad = false;
      rfsSlotInspect(&probe, idx, &hdr, NULL, &bad);

      if (bad)
      {
        outValidation->badSlotCount++;
      }
    }

    outValidation->committedCount = rfsCountActive(&probe);
    free(probe.slotBuf);
    free(probe.keyBufA);
    free(probe.keyBufB);
    free(probe.recBuf);
  }

  rfsIoClose(fd);

  char *oldPath = rfsStrdupWithSuffix(path, ".rfsold");
  char *newPath = rfsStrdupWithSuffix(path, ".rfsnew");

  if (oldPath != NULL && newPath != NULL)
  {
    outValidation->strayResizeFiles =
        rfsPathExists(oldPath) || rfsPathExists(newPath);
  }

  free(oldPath);
  free(newPath);
  return ESP_OK;
}

esp_err_t ringFileBreakStaleLock(const char *path)
{
  if (path == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }

  char *lockPath = rfsStrdupWithSuffix(path, ".lck");

  if (lockPath == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = ESP_OK;

  if (!rfsPathExists(lockPath))
  {
    err = ESP_ERR_NOT_FOUND;
  }
  else if (rfsIoUnlink(lockPath) != 0)
  {
    err = rfsErrFromErrno();
  }

  free(lockPath);
  return err;
}
