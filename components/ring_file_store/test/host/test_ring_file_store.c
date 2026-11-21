//--
//-- FileVersion: [7] 20-11-2026 - 22:20
//--
//-- ringFileStore - host-side automated tests for the ring_file_store
//-- component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
//-- The component sources are compiled against small POSIX shims
//-- (shim/) so the whole suite runs on the dev machine. Storage failures
//-- are injected by replacing the backend syscall table (rfs_file_io.h)
//-- around every write, sync and rename boundary, then inspecting the
//-- state after reopening.
//--
//-- Every test exercises the timestamp key model: the component stores
//-- [timeStamp][usedKey][payload] and returns those key fields on every
//-- read. Bucket order, period formulas and bucketId = period % capacity
//-- are checked against independent expected values.
//--
//-- Run via test/run_tests.sh.

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "ring_file_store.h"
#include "rfs_file_io.h"

//-- ---------------------------------------------------------------------------
//-- Failure injection layer
//-- ---------------------------------------------------------------------------

enum
{
  OP_OPEN,
  OP_CLOSE,
  OP_READ,
  OP_WRITE,
  OP_LSEEK,
  OP_FSYNC,
  OP_RENAME,
  OP_UNLINK,
  OP_COUNT
};

typedef struct
{
  char pat[128];
  int remaining;
} failRule_t;

#define RULE_SLOTS 4

static failRule_t sRules[OP_COUNT][RULE_SLOTS];
static char sDir[256];
static int sFailErrnoValue = EIO;

static void failErrno(int value)
{
  sFailErrnoValue = value;
}

static bool ruleMatch(const failRule_t *rule, const char *path)
{
  if (rule->remaining <= 0)
  {
    return false;
  }

  if (rule->pat[0] == '\0')
  {
    return true;
  }

  return path != NULL && strstr(path, rule->pat) != NULL;
}

//-- Fail the next count calls of an operation.
static void failNext(int op, int count)
{
  sRules[op][0].pat[0] = '\0';
  sRules[op][0].remaining = count;
}

//-- Fail the next count calls whose path argument contains pat. Each call
//-- takes the next free rule slot so several patterns can be active.
static void failNextOn(int op, int count, const char *pat)
{
  for (int i = 0; i < RULE_SLOTS; i++)
  {
    if (sRules[op][i].remaining <= 0)
    {
      sRules[op][i].remaining = count;
      snprintf(sRules[op][i].pat, sizeof(sRules[op][i].pat), "%s", pat);
      return;
    }
  }
}

static bool consumeFailure(int op, const char *path)
{
  for (int i = 0; i < RULE_SLOTS; i++)
  {
    if (ruleMatch(&sRules[op][i], path))
    {
      sRules[op][i].remaining--;
      errno = sFailErrnoValue;
      return true;
    }
  }

  return false;
}

static int hookOpen(const char *path, int flags, int mode)
{
  if (consumeFailure(OP_OPEN, path))
  {
    return -1;
  }

  return open(path, flags, (mode_t)mode);
}

static int hookClose(int fd)
{
  if (consumeFailure(OP_CLOSE, NULL))
  {
    return -1;
  }

  return close(fd);
}

static ssize_t hookRead(int fd, void *buf, size_t count)
{
  if (consumeFailure(OP_READ, NULL))
  {
    return -1;
  }

  return read(fd, buf, count);
}

static ssize_t hookWrite(int fd, const void *buf, size_t count)
{
  if (consumeFailure(OP_WRITE, NULL))
  {
    return -1;
  }

  return write(fd, buf, count);
}

static off_t hookLseek(int fd, off_t offset, int whence)
{
  if (consumeFailure(OP_LSEEK, NULL))
  {
    return (off_t)-1;
  }

  return lseek(fd, offset, whence);
}

static int hookFsync(int fd)
{
  if (consumeFailure(OP_FSYNC, NULL))
  {
    return -1;
  }

  return fsync(fd);
}

static int hookRename(const char *fromPath, const char *toPath)
{
  //-- Rules match either path of the rename so the first (path -> .rfsold)
  //-- and the second (.rfsnew -> path) switch step can fail independently.
  if (consumeFailure(OP_RENAME, fromPath) || consumeFailure(OP_RENAME, toPath))
  {
    return -1;
  }

  return rename(fromPath, toPath);
}

static int hookUnlink(const char *path)
{
  if (consumeFailure(OP_UNLINK, path))
  {
    return -1;
  }

  return unlink(path);
}

static void installHooks(void)
{
  rfsIoHooks_t hooks;
  memset(&hooks, 0, sizeof(hooks));
  hooks.open = hookOpen;
  hooks.close = hookClose;
  hooks.read = hookRead;
  hooks.write = hookWrite;
  hooks.lseek = hookLseek;
  hooks.fsync = hookFsync;
  hooks.rename = hookRename;
  hooks.unlink = hookUnlink;
  ringFileStoreSetIoHooks(&hooks);
  sFailErrnoValue = EIO;

  for (int op = 0; op < OP_COUNT; op++)
  {
    for (int i = 0; i < RULE_SLOTS; i++)
    {
      sRules[op][i].remaining = 0;
      sRules[op][i].pat[0] = '\0';
    }
  }
}

//-- ---------------------------------------------------------------------------
//-- Test scaffolding
//-- ---------------------------------------------------------------------------

static int gChecks = 0;
static int gFailures = 0;
static const char *gTestName = "";

#define CHECK(cond)                                                          \
  do                                                                         \
  {                                                                          \
    gChecks++;                                                               \
    if (!(cond))                                                             \
    {                                                                        \
      gFailures++;                                                           \
      printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, gTestName, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_ERR(expr, expected)                                                \
  do                                                                             \
  {                                                                              \
    esp_err_t e_ = (expr);                                                       \
    gChecks++;                                                                   \
    if (e_ != (expected))                                                        \
    {                                                                            \
      gFailures++;                                                               \
      printf("  FAIL %s:%d [%s] %s: got %s want %s\n", __FILE__, __LINE__,      \
             gTestName, #expr, esp_err_to_name(e_), esp_err_to_name(expected));  \
    }                                                                            \
  } while (0)

//-- Caller payload structs: never a key field, only fixed-width members.
typedef struct
{
  uint32_t energyWh;
  uint16_t errorCount;
  uint16_t pad;
} meterRec_t;

typedef struct
{
  uint32_t totalWh;
  uint16_t outages;
  uint16_t pad;
  uint32_t peakW;
} monthRec_t;

typedef struct
{
  uint32_t tempMilliC;
  uint16_t humidity;
  uint16_t pad;
} sensorRec_t;

//-- Read output structs: the automatic key fields, then the payload.
typedef struct
{
  ringFileKeyFields_t keys;
  meterRec_t payload;
} meterFull_t;

typedef struct
{
  ringFileKeyFields_t keys;
  monthRec_t payload;
} monthFull_t;

typedef struct
{
  ringFileKeyFields_t keys;
  sensorRec_t payload;
} sensorFull_t;

//-- ---------------------------------------------------------------------------
//-- Timestamp helpers
//-- ---------------------------------------------------------------------------

//-- Build a 14-digit EEYYMMDDhhmmss numeric timestamp.
static uint64_t mkTs(uint32_t year, uint32_t month, uint32_t day, uint32_t hh,
                     uint32_t mi, uint32_t ss)
{
  uint64_t v = year;
  v = v * 100u + month;
  v = v * 100u + day;
  v = v * 100u + hh;
  v = v * 100u + mi;
  v = v * 100u + ss;
  return v;
}

//-- usedKey of an hourly timestamp (truncate to the "YYMMDDhh" digits:
//-- drop the trailing mm and ss, 4 digits).
static uint64_t hourKey(uint64_t ts)
{
  return ts / 10000u;
}

//-- Independent period references (verified by hand against the documented
//-- formulas): days since 1970-01-01 for the fixed test dates.
#define DAYS_2026_11_18 20775u
#define DAYS_2026_10_23 20749u
#define DAYS_2024_02_29 19782u

//-- ---------------------------------------------------------------------------
//-- testLog.md writer: one "Command" line (with its data) followed by its
//-- "Result", plus ring dumps showing timeStamp, usedKey and the reported
//-- bucketId. Path comes from argv[1] (run_tests.sh passes the repository
//-- root).
//-- ---------------------------------------------------------------------------

static FILE *gLog = NULL;

static void logLine(const char *fmt, ...)
{
  if (gLog == NULL)
  {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(gLog, fmt, ap);
  va_end(ap);
  fprintf(gLog, "\n");
}

static void logCmd(esp_err_t result, const char *fmt, ...)
{
  if (gLog == NULL)
  {
    return;
  }

  char text[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  logLine("- **Command:** %s", text);
  logLine("  **Result:** %s", esp_err_to_name(result));
}

static void logCmdText(const char *resultText, const char *fmt, ...)
{
  if (gLog == NULL)
  {
    return;
  }

  char text[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  logLine("- **Command:** %s", text);
  logLine("  **Result:** %s", resultText);
}

static void logRingDump(ringFileHandle_t *h, const char *step)
{
  if (gLog == NULL)
  {
    return;
  }

  ringFileInfo_t info;
  memset(&info, 0, sizeof(info));
  ringFileGetInfo(h, &info);
  logLine("- **Command:** ring dump after \"%s\" (count=%lu capacity=%lu, oldest → newest)",
          step, (unsigned long)info.count, (unsigned long)info.capacity);

  ringFileIterator_t it;
  meterFull_t rec;
  memset(&it, 0, sizeof(it));
  memset(&rec, 0, sizeof(rec));

  if (ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &rec) != ESP_OK)
  {
    logLine("  **Result:** ring is empty");
    return;
  }

  for (;;)
  {
    uint32_t slot = RING_FILE_SLOT_STAGED;
    ringFileIteratorGetSlot(h, &it, &slot);

    if (slot == RING_FILE_SLOT_STAGED)
    {
      logLine("  - slot **staged** (not yet on storage) ts=%llu usedKey=%llu "
              "energyWh=%lu errorCount=%u",
              (unsigned long long)rec.keys.timeStamp,
              (unsigned long long)rec.keys.usedKey,
              (unsigned long)rec.payload.energyWh,
              (unsigned)rec.payload.errorCount);
    }
    else
    {
      logLine("  - bucketId %lu ts=%llu usedKey=%llu energyWh=%lu errorCount=%u",
              (unsigned long)slot, (unsigned long long)rec.keys.timeStamp,
              (unsigned long long)rec.keys.usedKey,
              (unsigned long)rec.payload.energyWh,
              (unsigned)rec.payload.errorCount);
    }

    if (ringFileIteratorNext(h, &it, &rec) != ESP_OK)
    {
      break;
    }
  }
}

//-- ---------------------------------------------------------------------------
//-- File/config helpers
//-- ---------------------------------------------------------------------------

static void ringPath(char *out, size_t size, const char *name)
{
  snprintf(out, size, "%s/%s.rfs", sDir, name);
}

static ringFileConfig_t baseConfig(uint32_t capacity, const char *schemaId,
                                   uint16_t payloadSize)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = capacity;
  cfg.schemaId = schemaId;
  cfg.recordSize = payloadSize;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileConfig_t meterConfig(uint32_t capacity)
{
  return baseConfig(capacity, RING_FILE_SCHEMA_YYMMDDHH, sizeof(meterRec_t));
}

static ringFileConfig_t monthConfig(uint32_t capacity)
{
  return baseConfig(capacity, RING_FILE_SCHEMA_YYMM, sizeof(monthRec_t));
}

static ringFileConfig_t sensorConfig(uint32_t capacity)
{
  return baseConfig(capacity, RING_FILE_SCHEMA_YYMMDDHHMMSS,
                    sizeof(sensorRec_t));
}

static ringFileOpenConfig_t baseOpen(const char *schemaId,
                                     uint32_t expectedPayload, bool readOnly,
                                     ringFilePersistMode_t mode,
                                     uint16_t maxPending)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = schemaId;
  cfg.expectedRecordSize = expectedPayload;
  cfg.readOnly = readOnly;
  cfg.persistMode = mode;
  cfg.maxPendingRecords = maxPending;
  return cfg;
}

static ringFileOpenConfig_t meterOpen(bool readOnly, ringFilePersistMode_t mode,
                                      uint16_t maxPending)
{
  return baseOpen(RING_FILE_SCHEMA_YYMMDDHH, sizeof(meterRec_t), readOnly,
                  mode, maxPending);
}

static void mkMeter(meterRec_t *m, uint32_t wh, uint16_t errs)
{
  memset(m, 0, sizeof(*m));
  m->energyWh = wh;
  m->errorCount = errs;
}

static void mkMonth(monthRec_t *m, uint32_t wh, uint16_t outages, uint32_t peakW)
{
  memset(m, 0, sizeof(*m));
  m->totalWh = wh;
  m->outages = outages;
  m->peakW = peakW;
}

static void mkSensor(sensorRec_t *m, uint32_t tempMilliC, uint16_t humidity)
{
  memset(m, 0, sizeof(*m));
  m->tempMilliC = tempMilliC;
  m->humidity = humidity;
}

static ringFileHandle_t *mustCreateCfg(const char *name,
                                       const ringFileConfig_t *cfg)
{
  char path[300];
  ringPath(path, sizeof(path), name);
  ringFileHandle_t *h = NULL;
  esp_err_t err = ringFileCreate(path, cfg, &h);

  if (err != ESP_OK)
  {
    printf("  FATAL create %s: %s\n", name, esp_err_to_name(err));
    exit(2);
  }

  return h;
}

static ringFileHandle_t *mustCreateMeter(const char *name, uint32_t capacity)
{
  ringFileConfig_t cfg = meterConfig(capacity);
  return mustCreateCfg(name, &cfg);
}

static ringFileHandle_t *mustOpenCfg(const char *name,
                                     const ringFileOpenConfig_t *cfg)
{
  char path[300];
  ringPath(path, sizeof(path), name);
  ringFileHandle_t *h = NULL;
  esp_err_t err = ringFileOpen(path, cfg, &h);

  if (err != ESP_OK)
  {
    printf("  FATAL open %s: %s\n", name, esp_err_to_name(err));
    exit(2);
  }

  return h;
}

static ringFileHandle_t *mustOpenMeterMode(const char *name, bool readOnly,
                                           ringFilePersistMode_t mode,
                                           uint16_t maxPending)
{
  ringFileOpenConfig_t cfg = meterOpen(readOnly, mode, maxPending);
  return mustOpenCfg(name, &cfg);
}

static ringFileHandle_t *mustOpenMeter(const char *name, bool readOnly)
{
  return mustOpenMeterMode(name, readOnly, RING_FILE_PERSIST_IMMEDIATE, 0);
}

static uint32_t infoCount(ringFileHandle_t *h)
{
  ringFileInfo_t info;
  memset(&info, 0, sizeof(info));

  if (ringFileGetInfo(h, &info) != ESP_OK)
  {
    return 0xFFFFFFFFu;
  }

  return info.count;
}

//-- Add one hourly record for (day 23 + dayOff, hour hh) of October 2026.
static esp_err_t addHour(ringFileHandle_t *h, uint32_t dayOff, uint32_t hh,
                         uint32_t wh)
{
  meterRec_t rec;
  mkMeter(&rec, wh, (uint16_t)hh);
  uint64_t ts = mkTs(2026u, 10u, 23u + dayOff, hh, 0u, 0u);
  esp_err_t err = ringFileAdd(h, ts, &rec);
  logCmd(err, "ringFileAdd(ts=%014llu usedKey=%llu energyWh=%lu)",
         (unsigned long long)ts, (unsigned long long)hourKey(ts),
         (unsigned long)wh);
  return err;
}

static uint64_t hourTs(uint32_t dayOff, uint32_t hh)
{
  return mkTs(2026u, 10u, 23u + dayOff, hh, 0u, 0u);
}

static bool pathExists(const char *path)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0)
  {
    return false;
  }

  close(fd);
  return true;
}

static void pokeFile(const char *path, size_t offset, const uint8_t *bytes,
                     size_t len)
{
  int fd = open(path, O_WRONLY);

  if (fd < 0)
  {
    printf("  FATAL poke open %s\n", path);
    exit(2);
  }

  if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset ||
      write(fd, bytes, len) != (ssize_t)len)
  {
    printf("  FATAL poke write %s\n", path);
    exit(2);
  }

  close(fd);
}

#define SLOT_HDR_SIZE 20u
#define HEADER_SIZE 96u

//-- CRC32 mirror of the component's header checksum (reflected polynomial
//-- 0xEDB88320), used to keep the header CRC valid while poking fields.
static uint32_t testCrc32(const uint8_t *data, size_t len)
{
  uint32_t state = 0xFFFFFFFFu;

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

  return ~state;
}

static void readWhole(const char *path, uint8_t *buf, size_t len)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0 || pread(fd, buf, len, 0) != (ssize_t)len)
  {
    printf("  FATAL read %s\n", path);
    exit(2);
  }

  close(fd);
}

//-- Poke a header field and repair the CRC over [0,88) so the decode
//-- reaches the field check instead of the checksum check.
static void pokeHeader(const char *path, size_t offset, const uint8_t *bytes,
                       size_t len)
{
  uint8_t hdr[HEADER_SIZE];
  readWhole(path, hdr, sizeof(hdr));
  memcpy(hdr + offset, bytes, len);
  uint32_t crc = testCrc32(hdr, 88);
  hdr[88] = (uint8_t)(crc & 0xFFu);
  hdr[89] = (uint8_t)((crc >> 8) & 0xFFu);
  hdr[90] = (uint8_t)((crc >> 16) & 0xFFu);
  hdr[91] = (uint8_t)((crc >> 24) & 0xFFu);
  pokeFile(path, 0, hdr, sizeof(hdr));
}

static void copyFile(const char *src, const char *dst)
{
  int sfd = open(src, O_RDONLY);
  int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  uint8_t buf[4096];
  ssize_t n;

  while ((n = read(sfd, buf, sizeof(buf))) > 0)
  {
    if (write(dfd, buf, (size_t)n) != n)
    {
      break;
    }
  }

  close(sfd);
  close(dfd);
}

static size_t slotRecordOffset(uint32_t index, uint32_t recordSize)
{
  return HEADER_SIZE + (size_t)index * (SLOT_HDR_SIZE + recordSize) +
         SLOT_HDR_SIZE;
}

static rfsIoStats_t lastStats(void)
{
  rfsIoStats_t stats;
  ringFileStoreGetIoStats(&stats);
  return stats;
}

//-- ---------------------------------------------------------------------------
//-- Tests
//-- ---------------------------------------------------------------------------

static void testArguments(void)
{
  char path[300];
  ringPath(path, sizeof(path), "args");
  ringFileHandle_t *h = NULL;
  ringFileConfig_t cfg = meterConfig(4);

  CHECK_ERR(ringFileCreate(NULL, &cfg, &h), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileCreate(path, NULL, &h), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileCreate(path, &cfg, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileCreate("", &cfg, &h), ESP_ERR_INVALID_ARG);

  ringFileConfig_t bad = meterConfig(4);
  bad.maxRecords = 0;
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(65537u);
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.recordSize = 0;
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.schemaId = NULL;
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.schemaId = "";
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.schemaId = "XX";
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  //-- File types are case-sensitive: hh/mm/ss must be lower case.
  bad = meterConfig(4);
  bad.schemaId = "YYMMDDHH";
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.schemaId = "WEEK";
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.persistMode = (ringFilePersistMode_t)7;
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  bad = meterConfig(4);
  bad.persistMode = RING_FILE_PERSIST_BUFFERED;
  bad.maxPendingRecords = 65;
  CHECK_ERR(ringFileCreate(path, &bad, &h), ESP_ERR_INVALID_ARG);

  CHECK(!pathExists(path));

  ringFileHandle_t *w = mustCreateMeter("argsOk", 4);
  meterRec_t rec;
  mkMeter(&rec, 1000, 0);

  CHECK_ERR(ringFileAdd(NULL, 20261023140000ULL, &rec), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(w, 20261023140000ULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileReadByKey(NULL, 2026102314ULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileReadByKey(w, 2026102314ULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUpdateByKey(w, 2026102314ULL, 20261023140000ULL, NULL),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUpsert(w, 20261023140000ULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileEdit(w, 20261023140000ULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileReadOldest(w, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileReadNewest(w, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileGetInfo(NULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileResize(w, 0), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileValidate(NULL, NULL), ESP_ERR_INVALID_ARG);

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(w, &it, RING_FILE_ITER_OLDEST, NULL, NULL),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileIteratorBegin(w, NULL, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileIteratorBegin(w, &it, RING_FILE_ITER_KEY, NULL, &full),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileIteratorGetSlot(w, &it, NULL), ESP_ERR_INVALID_ARG);

  ringFileClose(w);
}

static void testCreateOpenReopen(void)
{
  ringFileHandle_t *h = mustCreateMeter("reopen", 4);

  for (uint32_t hh = 0; hh < 3; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 100u + hh), ESP_OK);
  }

  logRingDump(h, "add hh0..hh2");

  ringFileInfo_t info;
  memset(&info, 0, sizeof(info));
  CHECK_ERR(ringFileGetInfo(h, &info), ESP_OK);
  CHECK(info.count == 3);
  CHECK(info.capacity == 4);
  CHECK(info.payloadSize == sizeof(meterRec_t));
  CHECK(info.recordSize == sizeof(meterFull_t));
  CHECK(info.storageSize == HEADER_SIZE + 5u * (SLOT_HDR_SIZE + 24u));
  CHECK(info.pendingCount == 0);
  CHECK(!info.dirty);
  CHECK(strcmp(info.schemaId, RING_FILE_SCHEMA_YYMMDDHH) == 0);
  CHECK(info.granularity == RING_FILE_GRANULARITY_HOUR);

  ringFileClose(h);

  h = mustOpenMeter("reopen", false);
  CHECK(infoCount(h) == 3);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0, 0));
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 0)));
  CHECK(full.payload.energyWh == 100);

  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0, 2));
  CHECK(full.payload.energyWh == 102);

  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadByKey(h, hourKey(hourTs(0, 1)), &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0, 1));
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 1)));
  CHECK(full.payload.energyWh == 101);

  ringFileClose(h);
}

static void testEmptyRing(void)
{
  ringFileHandle_t *h = mustCreateMeter("empty", 3);
  meterFull_t full;
  memset(&full, 0, sizeof(full));

  CHECK_ERR(ringFileReadOldest(h, &full), RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileReadNewest(h, &full), RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileReadByKey(h, 2026102314ULL, &full),
            RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_NEWEST, NULL, &full),
            RING_FILE_STORE_ERR_RING_EMPTY);
  uint64_t key = hourKey(hourTs(0, 0));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY, &key, &full),
            RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  CHECK(infoCount(h) == 0);
  ringFileClose(h);
}

static void testSingleSlotRing(void)
{
  ringFileHandle_t *h = mustCreateMeter("single", 1);

  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  CHECK(infoCount(h) == 1);

  //-- Same bucket, later timestamp: update in place, count unchanged.
  meterRec_t rec;
  mkMeter(&rec, 11, 9);
  CHECK_ERR(ringFileUpsert(h, mkTs(2026u, 10u, 23u, 0u, 30u, 0u), &rec), ESP_OK);
  CHECK(infoCount(h) == 1);

  //-- Newer bucket evicts the single old one.
  CHECK_ERR(addHour(h, 0, 1, 20), ESP_OK);
  CHECK(infoCount(h) == 1);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 1)));
  CHECK(full.payload.energyWh == 20);

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  uint32_t slot = 0xFFFFFFFFu;
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
  CHECK(slot == 0u);

  ringFileClose(h);
}

static void testSameBucketUpdate(void)
{
  ringFileHandle_t *h = mustCreateMeter("samebucket", 3);

  CHECK_ERR(addHour(h, 0, 1, 100), ESP_OK);
  logRingDump(h, "add hh1");

  //-- A second timestamp inside the same hour updates the active bucket.
  meterRec_t rec;
  mkMeter(&rec, 150, 3);
  CHECK_ERR(ringFileUpsert(h, mkTs(2026u, 10u, 23u, 1u, 30u, 0u), &rec), ESP_OK);
  CHECK(infoCount(h) == 1);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadByKey(h, hourKey(hourTs(0, 1)), &full), ESP_OK);
  CHECK(full.keys.timeStamp == mkTs(2026u, 10u, 23u, 1u, 30u, 0u));
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 1)));
  CHECK(full.payload.energyWh == 150);
  CHECK(full.payload.errorCount == 3);

  logRingDump(h, "upsert hh1:30");

  ringFileClose(h);
}

static void testNotNewer(void)
{
  ringFileHandle_t *h = mustCreateMeter("notnewer", 2);

  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  CHECK_ERR(addHour(h, 0, 1, 20), ESP_OK);
  CHECK_ERR(addHour(h, 0, 2, 30), ESP_OK);
  logRingDump(h, "add hh0..hh2 (capacity 2, hh0 evicted)");

  //-- hh0 is evicted and older than the newest bucket: rejected.
  CHECK_ERR(addHour(h, 0, 0, 99), RING_FILE_STORE_ERR_NOT_NEWER);
  CHECK(infoCount(h) == 2);

  //-- hh1 is still an active bucket: add updates it in place.
  CHECK_ERR(addHour(h, 0, 1, 25), ESP_OK);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 1)));
  CHECK(full.payload.energyWh == 25);

  ringFileClose(h);
}

static void testTimestampValidation(void)
{
  ringFileHandle_t *h = mustCreateMeter("tsvalid", 3);
  meterRec_t rec;
  mkMeter(&rec, 1, 0);

  //-- Invalid digits for a "YYMMDDhh" file are rejected without changes.
  CHECK_ERR(ringFileAdd(h, mkTs(2026u, 13u, 1u, 1u, 0u, 0u), &rec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, mkTs(2026u, 4u, 31u, 1u, 0u, 0u), &rec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, mkTs(2026u, 10u, 23u, 24u, 0u, 0u), &rec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, mkTs(2026u, 10u, 23u, 1u, 60u, 0u), &rec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, mkTs(2026u, 10u, 23u, 1u, 0u, 60u), &rec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, 19961023010000ULL, &rec), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, 21261023010000ULL, &rec), ESP_ERR_INVALID_ARG);
  CHECK(infoCount(h) == 0);

  CHECK_ERR(ringFileTimeStampToUsedKey(mkTs(2026u, 2u, 29u, 0u, 0u, 0u),
                                       RING_FILE_GRANULARITY_HOUR, NULL),
            ESP_ERR_INVALID_ARG);

  uint64_t period = 0;
  CHECK_ERR(ringFileTimeStampToPeriod(19991231235959ULL,
                                      RING_FILE_GRANULARITY_DAY, &period),
            ESP_ERR_INVALID_ARG);

  //-- The ring is still usable after the rejected adds.
  CHECK_ERR(addHour(h, 0, 0, 7), ESP_OK);
  CHECK(infoCount(h) == 1);

  ringFileClose(h);
}

static void testBucketAndKeyDerivation(void)
{
  uint64_t key = 0;
  uint64_t period = 0;
  ringFileGranularity_t gran = RING_FILE_GRANULARITY_NONE;

  //-- File type strings map to the documented granularity.
  CHECK_ERR(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YY, &gran), ESP_OK);
  CHECK(gran == RING_FILE_GRANULARITY_YEAR);
  CHECK_ERR(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YYMM, &gran), ESP_OK);
  CHECK(gran == RING_FILE_GRANULARITY_MONTH);
  CHECK_ERR(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YYMMDD, &gran), ESP_OK);
  CHECK(gran == RING_FILE_GRANULARITY_DAY);
  CHECK_ERR(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YYMMDDHH, &gran), ESP_OK);
  CHECK(gran == RING_FILE_GRANULARITY_HOUR);
  CHECK_ERR(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YYMMDDHHMM, &gran), ESP_OK);
  CHECK(gran == RING_FILE_GRANULARITY_MINUTE);
  CHECK_ERR(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YYMMDDHHMMSS, &gran), ESP_OK);
  CHECK(gran == RING_FILE_GRANULARITY_SECOND);
  CHECK_ERR(ringFileGranularityFromSchemaId(NULL, &gran), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileGranularityFromSchemaId("YYmm", &gran), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileGranularityFromSchemaId("WEEK", &gran), ESP_ERR_INVALID_ARG);

  //-- "YY": usedKey 2026, period = year.
  CHECK_ERR(ringFileTimeStampToUsedKey(20260000000000ULL,
                                       RING_FILE_GRANULARITY_YEAR, &key), ESP_OK);
  CHECK(key == 2026ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20260000000000ULL,
                                      RING_FILE_GRANULARITY_YEAR, &period), ESP_OK);
  CHECK(period == 2026ULL);

  //-- "YYMM": month formula (year - 1) * 12 + month.
  CHECK_ERR(ringFileTimeStampToUsedKey(20261000000000ULL,
                                       RING_FILE_GRANULARITY_MONTH, &key), ESP_OK);
  CHECK(key == 202610ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20261000000000ULL,
                                      RING_FILE_GRANULARITY_MONTH, &period), ESP_OK);
  CHECK(period == 24310ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20260101000000ULL,
                                      RING_FILE_GRANULARITY_MONTH, &period), ESP_OK);
  CHECK(period == 24301ULL);

  //-- "YYMMDD": days since 1970-01-01, zero padding and valid sub-fields.
  CHECK_ERR(ringFileTimeStampToUsedKey(20261118000000ULL,
                                       RING_FILE_GRANULARITY_DAY, &key), ESP_OK);
  CHECK(key == 20261118ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20261118000000ULL,
                                      RING_FILE_GRANULARITY_DAY, &period), ESP_OK);
  CHECK(period == DAYS_2026_11_18);
  CHECK_ERR(ringFileTimeStampToUsedKey(20261118000001ULL,
                                       RING_FILE_GRANULARITY_DAY, &key), ESP_OK);
  CHECK(key == 20261118ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20240229000000ULL,
                                      RING_FILE_GRANULARITY_DAY, &period), ESP_OK);
  CHECK(period == DAYS_2024_02_29);
  CHECK_ERR(ringFileTimeStampToPeriod(20260229000000ULL,
                                      RING_FILE_GRANULARITY_DAY, &period),
            ESP_ERR_INVALID_ARG);

  //-- "YYMMDDhh": days * 24 + hour.
  CHECK_ERR(ringFileTimeStampToUsedKey(mkTs(2026u, 11u, 18u, 14u, 0u, 0u),
                                       RING_FILE_GRANULARITY_HOUR, &key), ESP_OK);
  CHECK(key == 2026111814ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 14u, 0u, 0u),
                                      RING_FILE_GRANULARITY_HOUR, &period), ESP_OK);
  CHECK(period == DAYS_2026_11_18 * 24ULL + 14ULL);

  //-- "YYMMDDhhmm": days * 1440 + hour * 60 + minute.
  CHECK_ERR(ringFileTimeStampToUsedKey(mkTs(2026u, 11u, 18u, 14u, 30u, 0u),
                                       RING_FILE_GRANULARITY_MINUTE, &key), ESP_OK);
  CHECK(key == 202611181430ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 14u, 30u, 0u),
                                      RING_FILE_GRANULARITY_MINUTE, &period), ESP_OK);
  CHECK(period == DAYS_2026_11_18 * 1440ULL + 14ULL * 60ULL + 30ULL);

  //-- "YYMMDDhhmmss": days * 86400 + seconds of day.
  CHECK_ERR(ringFileTimeStampToUsedKey(mkTs(2026u, 11u, 18u, 14u, 30u, 55u),
                                       RING_FILE_GRANULARITY_SECOND, &key), ESP_OK);
  CHECK(key == 20261118143055ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 14u, 30u, 55u),
                                      RING_FILE_GRANULARITY_SECOND, &period), ESP_OK);
  CHECK(period == DAYS_2026_11_18 * 86400ULL + 14ULL * 3600ULL + 30ULL * 60ULL + 55ULL);

  //-- A partial current period never counts as a full unit: the period
  //-- counts only COMPLETE days/hours/minutes/seconds before the timestamp,
  //-- so it advances by exactly one when a period rolls over. The current
  //-- day contributes its completed sub-periods only, never a full 24 h.
  uint64_t periodA = 0;
  uint64_t periodB = 0;

  //-- Day rolls over at midnight: the new day starts at the next number.
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 23u, 59u, 59u),
                                      RING_FILE_GRANULARITY_DAY, &periodA), ESP_OK);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 19u, 0u, 0u, 0u),
                                      RING_FILE_GRANULARITY_DAY, &periodB), ESP_OK);
  CHECK(periodA == DAYS_2026_11_18);
  CHECK(periodB == DAYS_2026_11_18 + 1ULL);

  //-- Hour rolls over at midnight: 23:00 is the last hourly bucket of the
  //-- day; 00:00 the next day is exactly one period later. At 14:00 on
  //-- day D+1 the day contributes 24 completed hours + 14, not 48.
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 23u, 59u, 0u),
                                      RING_FILE_GRANULARITY_HOUR, &periodA), ESP_OK);
  CHECK(periodA == DAYS_2026_11_18 * 24ULL + 23ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 19u, 0u, 0u, 0u),
                                      RING_FILE_GRANULARITY_HOUR, &periodB), ESP_OK);
  CHECK(periodB == periodA + 1ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 19u, 14u, 0u, 0u),
                                      RING_FILE_GRANULARITY_HOUR, &periodB), ESP_OK);
  CHECK(periodB == (DAYS_2026_11_18 + 1ULL) * 24ULL + 14ULL);

  //-- Minute and second roll over the same way.
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 23u, 59u, 0u),
                                      RING_FILE_GRANULARITY_MINUTE, &periodA), ESP_OK);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 19u, 0u, 0u, 0u),
                                      RING_FILE_GRANULARITY_MINUTE, &periodB), ESP_OK);
  CHECK(periodB == periodA + 1ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 18u, 23u, 59u, 59u),
                                      RING_FILE_GRANULARITY_SECOND, &periodA), ESP_OK);
  CHECK_ERR(ringFileTimeStampToPeriod(mkTs(2026u, 11u, 19u, 0u, 0u, 0u),
                                      RING_FILE_GRANULARITY_SECOND, &periodB), ESP_OK);
  CHECK(periodB == periodA + 1ULL);

  //-- Month rolls over at the December -> January boundary with the
  //-- month formula: (year - 1) * 12 + month counts the months of
  //-- the previous years plus the index of the current month.
  CHECK_ERR(ringFileTimeStampToPeriod(20261231000000ULL,
                                      RING_FILE_GRANULARITY_MONTH, &periodA), ESP_OK);
  CHECK(periodA == 24312ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20270101000000ULL,
                                      RING_FILE_GRANULARITY_MONTH, &periodB), ESP_OK);
  CHECK(periodB == periodA + 1ULL);

  //-- Year rolls over: period advances by one at 31-12 -> 01-01.
  CHECK_ERR(ringFileTimeStampToPeriod(20261231000000ULL,
                                      RING_FILE_GRANULARITY_YEAR, &periodA), ESP_OK);
  CHECK(periodA == 2026ULL);
  CHECK_ERR(ringFileTimeStampToPeriod(20270101000000ULL,
                                      RING_FILE_GRANULARITY_YEAR, &periodB), ESP_OK);
  CHECK(periodB == periodA + 1ULL);

  //-- Integration: the reported bucketId is period % capacity. For the test
  //-- dates the hourly period is DAYS_2026_10_23 * 24 + hh = 497976 + hh.
  ringFileHandle_t *h = mustCreateMeter("bucket5", 5);

  for (uint32_t hh = 0; hh < 5; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 100u + hh), ESP_OK);
  }

  logRingDump(h, "add hh0..hh4 (capacity 5)");

  ringFileIterator_t it;
  meterFull_t full;
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);

  uint32_t seen = 0;

  for (;;)
  {
    uint32_t slot = 0;
    CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
    uint64_t expectPeriod = DAYS_2026_10_23 * 24ULL + (uint64_t)seen;
    CHECK(slot == (uint32_t)(expectPeriod % 5u));
    CHECK(full.keys.usedKey == hourKey(hourTs(0, seen)));
    seen++;

    if (ringFileIteratorNext(h, &it, &full) != ESP_OK)
    {
      break;
    }
  }

  CHECK(seen == 5);
  ringFileClose(h);

  //-- "YYMM" ring: the October 2026 period 24310 maps to bucket 10 of 12.
  ringFileConfig_t mcfg = monthConfig(12);
  ringFileHandle_t *m = mustCreateCfg("bucket12", &mcfg);

  monthRec_t monthRec;
  mkMonth(&monthRec, 4000, 1, 3000);
  CHECK_ERR(ringFileAdd(m, 20261000000000ULL, &monthRec), ESP_OK);
  mkMonth(&monthRec, 5000, 2, 3400);
  CHECK_ERR(ringFileAdd(m, 20261100000000ULL, &monthRec), ESP_OK);
  logCmd(ESP_OK, "ringFileAdd(months 202610, 202611)");

  ringFileIterator_t mit;
  memset(&mit, 0, sizeof(mit));
  monthFull_t mfull;
  memset(&mfull, 0, sizeof(mfull));
  CHECK_ERR(ringFileIteratorBegin(m, &mit, RING_FILE_ITER_NEWEST, NULL, &mfull),
            ESP_OK);
  uint32_t mslot = 0;
  CHECK_ERR(ringFileIteratorGetSlot(m, &mit, &mslot), ESP_OK);
  CHECK(mslot == 24311u % 12u);
  CHECK(mfull.keys.usedKey == 202611ULL);
  CHECK(mfull.payload.totalWh == 5000);
  ringFileClose(m);
}

static void testEvictionOrder(void)
{
  ringFileHandle_t *h = mustCreateMeter("evict", 3);

  for (uint32_t hh = 0; hh < 5; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u * hh), ESP_OK);
  }

  CHECK(infoCount(h) == 3);
  logRingDump(h, "add hh0..hh4 (capacity 3)");

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 2)));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 4)));

  ringFileClose(h);

  h = mustOpenMeter("evict", false);
  CHECK(infoCount(h) == 3);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 2)));
  ringFileClose(h);
}

static void testEditWindow(void)
{
  ringFileHandle_t *h = mustCreateMeter("editwin", 3);

  for (uint32_t hh = 0; hh < 3; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u + hh), ESP_OK);
  }

  //-- An empty ring accepts any bucket.
  ringFileHandle_t *e = mustCreateMeter("editwin2", 3);
  meterRec_t rec;
  mkMeter(&rec, 1, 0);
  CHECK_ERR(ringFileEdit(e, hourTs(0, 10), &rec), ESP_OK);
  CHECK(infoCount(e) == 1);
  ringFileClose(e);

  //-- Edit of an active bucket with any timestamp: update in place.
  meterRec_t upd;
  mkMeter(&upd, 77, 0);
  CHECK_ERR(ringFileEdit(h, mkTs(2026u, 10u, 23u, 2u, 45u, 0u), &upd), ESP_OK);
  CHECK(infoCount(h) == 3);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadByKey(h, hourKey(hourTs(0, 2)), &full), ESP_OK);
  CHECK(full.keys.timeStamp == mkTs(2026u, 10u, 23u, 2u, 45u, 0u));
  CHECK(full.payload.energyWh == 77);

  //-- Insert a newer bucket through edit (evicts the oldest).
  meterRec_t ins;
  mkMeter(&ins, 55, 0);
  CHECK_ERR(ringFileEdit(h, hourTs(0, 5), &ins), ESP_OK);
  CHECK(infoCount(h) == 3);
  logRingDump(h, "edit hh5 insert (evicts hh0)");

  //-- The evicted hh0 period is below the retained window now.
  CHECK_ERR(ringFileEdit(h, hourTs(0, 0), &ins),
            RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE);
  CHECK(infoCount(h) == 3);

  ringFileClose(h);
}

static void testUpdateByKeyRules(void)
{
  ringFileHandle_t *h = mustCreateMeter("updkey", 3);

  CHECK_ERR(addHour(h, 0, 1, 100), ESP_OK);

  //-- Correct lookup key and a timestamp deriving to it: update.
  meterRec_t rec;
  mkMeter(&rec, 200, 5);
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 1)), hourTs(0, 1), &rec),
            ESP_OK);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadByKey(h, hourKey(hourTs(0, 1)), &full), ESP_OK);
  CHECK(full.payload.energyWh == 200);

  //-- Position preserved: bucketId equals the derived period modulo 3.
  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY,
                                  &(uint64_t){hourKey(hourTs(0, 1))}, &full),
            ESP_OK);
  uint32_t slot = 0;
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
  uint64_t period = DAYS_2026_10_23 * 24ULL + 1ULL;
  CHECK(slot == (uint32_t)(period % 3u));

  //-- Mismatched derived key: INVALID_ARG, nothing changed.
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 1)), hourTs(0, 2), &rec),
            ESP_ERR_INVALID_ARG);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadByKey(h, hourKey(hourTs(0, 2)), &full),
            RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  //-- Unknown lookup key: KEY_NOT_FOUND.
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 5)), hourTs(0, 5), &rec),
            RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  //-- Invalid timestamp digits: INVALID_ARG.
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 1)), 20261399010000ULL,
                                &rec), ESP_ERR_INVALID_ARG);

  ringFileClose(h);
}

static void testIterator(void)
{
  ringFileHandle_t *h = mustCreateMeter("iter", 4);

  for (uint32_t hh = 0; hh < 4; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u + hh), ESP_OK);
  }

  ringFileIterator_t it;
  meterFull_t full;

  //-- Forward from the oldest.
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  CHECK(full.payload.energyWh == 10);

  for (uint32_t expect = 11; expect <= 13; expect++)
  {
    memset(&full, 0, sizeof(full));
    CHECK_ERR(ringFileIteratorNext(h, &it, &full), ESP_OK);
    CHECK(full.payload.energyWh == expect);
  }

  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorNext(h, &it, &full),
            RING_FILE_STORE_ERR_ITER_END);

  //-- Backward from the newest.
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_NEWEST, NULL, &full),
            ESP_OK);
  CHECK(full.payload.energyWh == 13);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &full), ESP_OK);
  CHECK(full.payload.energyWh == 12);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &full), ESP_OK);
  CHECK(full.payload.energyWh == 11);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &full), ESP_OK);
  CHECK(full.payload.energyWh == 10);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &full),
            RING_FILE_STORE_ERR_ITER_END);

  //-- Start at a key.
  uint64_t key = hourKey(hourTs(0, 2));
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY, &key, &full),
            ESP_OK);
  CHECK(full.keys.usedKey == key);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorNext(h, &it, &full), ESP_OK);
  CHECK(full.payload.energyWh == 13);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorNext(h, &it, &full),
            RING_FILE_STORE_ERR_ITER_END);

  uint64_t unknown = hourKey(hourTs(0, 9));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY, &unknown, &full),
            RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  //-- A successful mutation invalidates the cursor, slot is kept.
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  uint32_t slotBefore = 0;
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slotBefore), ESP_OK);
  CHECK_ERR(addHour(h, 0, 4, 14), ESP_OK);
  CHECK_ERR(ringFileIteratorNext(h, &it, &full),
            RING_FILE_STORE_ERR_ITER_INVALIDATED);
  //-- GetSlot on an invalidated cursor reports the error and leaves the
  //-- caller's slot value untouched (the last good slot is kept).
  uint32_t slotAfter = slotBefore;
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slotAfter),
            RING_FILE_STORE_ERR_ITER_INVALIDATED);
  CHECK(slotAfter == slotBefore);

  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  CHECK(full.payload.energyWh == 11);

  ringFileClose(h);
}

static void testSchemaMismatch(void)
{
  ringFileHandle_t *h = mustCreateMeter("schema", 3);
  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  ringFileClose(h);

  char path[300];
  ringPath(path, sizeof(path), "schema");
  ringFileHandle_t *o = NULL;

  ringFileOpenConfig_t cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  cfg.expectedSchemaId = RING_FILE_SCHEMA_YYMM;
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  cfg.expectedRecordSize = sizeof(monthRec_t);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  cfg.expectedSchemaId = NULL;
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), ESP_OK);
  CHECK(infoCount(o) == 1);
  ringFileClose(o);
}

static void testReadOnly(void)
{
  ringFileHandle_t *h = mustCreateMeter("ro", 3);
  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  ringFileClose(h);

  h = mustOpenMeter("ro", true);
  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.payload.energyWh == 10);

  meterRec_t rec;
  mkMeter(&rec, 99, 0);
  CHECK_ERR(ringFileAdd(h, hourTs(0, 1), &rec), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileUpsert(h, hourTs(0, 1), &rec), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileEdit(h, hourTs(0, 1), &rec), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 0)), hourTs(0, 0), &rec),
            RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileClear(h), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileResize(h, 5), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileSync(h), ESP_OK);

  ringFileClose(h);
}

static void testStaleLockBreak(void)
{
  ringFileHandle_t *h = mustCreateMeter("lock", 3);
  char path[300];
  ringPath(path, sizeof(path), "lock");
  ringFileHandle_t *o = NULL;
  ringFileOpenConfig_t cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);

  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_BUSY);
  CHECK_ERR(ringFileBreakStaleLock(path), ESP_OK);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), ESP_OK);
  CHECK_ERR(ringFileBreakStaleLock(path), ESP_OK);
  ringFileClose(o);

  //-- The lock file is removed on close, so breaking it finds nothing.
  ringFileClose(h);
  CHECK_ERR(ringFileBreakStaleLock(path), ESP_ERR_NOT_FOUND);
}

static void testCorruption(void)
{
  ringFileHandle_t *h = mustCreateMeter("corrupt", 3);

  for (uint32_t hh = 0; hh < 3; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u + hh), ESP_OK);
  }

  ringFileClose(h);

  char path[300];
  char good[340];
  ringPath(path, sizeof(path), "corrupt");
  snprintf(good, sizeof(good), "%s.good", path);
  copyFile(path, good);

  uint8_t junk[4] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
  ringFileHandle_t *o = NULL;
  ringFileOpenConfig_t cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);

  //-- A corrupted record byte hides only that slot on reopen.
  copyFile(good, path);
  uint8_t flip = 0xFFu;
  pokeFile(path, slotRecordOffset(1, 24) + 16, &flip, 1);
  h = mustOpenMeter("corrupt", false);
  CHECK(infoCount(h) == 2);
  ringFileClose(h);

  //-- Header magic: format error, reported by validate.
  copyFile(good, path);
  pokeFile(path, 0, junk, 4);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_FORMAT);
  ringFileValidation_t val;
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(!val.headerValid);
  CHECK(val.metadataIssue == RING_FILE_META_BAD_MAGIC);

  //-- Header checksum: corrupt error.
  copyFile(good, path);
  pokeFile(path, 90, junk, 4);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_CORRUPT);
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.metadataIssue == RING_FILE_META_BAD_CHECKSUM);

  //-- Unsupported format version (header CRC repaired so the version is
  //-- the first thing decode rejects).
  copyFile(good, path);
  uint8_t v3[2] = {3, 0};
  pokeHeader(path, 4, v3, 2);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_FORMAT);
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.metadataIssue == RING_FILE_META_BAD_FORMAT_VERSION);

  //-- Impossible granularity byte.
  copyFile(good, path);
  uint8_t g9[4] = {9, 0, 0, 0};
  pokeHeader(path, 40, g9, 4);
  CHECK_ERR(ringFileOpen(path, &cfg, &o), RING_FILE_STORE_ERR_FORMAT);
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.metadataIssue == RING_FILE_META_BAD_GEOMETRY);

  //-- The ring is healthy again after restoring the good copy.
  copyFile(good, path);
  h = mustOpenMeter("corrupt", false);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);

  unlink(good);
}

static void testClear(void)
{
  ringFileHandle_t *h = mustCreateMeter("clear", 3);

  for (uint32_t hh = 0; hh < 3; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u + hh), ESP_OK);
  }

  CHECK_ERR(ringFileClear(h), ESP_OK);
  CHECK(infoCount(h) == 0);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), RING_FILE_STORE_ERR_RING_EMPTY);

  //-- Adding after a clear works and the clear survives a reopen.
  CHECK_ERR(addHour(h, 0, 7, 70), ESP_OK);
  CHECK(infoCount(h) == 1);
  ringFileClose(h);

  h = mustOpenMeter("clear", false);
  CHECK(infoCount(h) == 1);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 7)));
  ringFileClose(h);
}

static void testResize(void)
{
  ringFileHandle_t *h = mustCreateMeter("resize", 5);

  for (uint32_t hh = 0; hh < 5; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u * hh), ESP_OK);
  }

  CHECK_ERR(ringFileResize(h, 3), ESP_OK);
  CHECK(infoCount(h) == 3);
  logRingDump(h, "resize 5 -> 3");

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 2)));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 4)));

  ringFileInfo_t info;
  memset(&info, 0, sizeof(info));
  CHECK_ERR(ringFileGetInfo(h, &info), ESP_OK);
  CHECK(info.capacity == 3);
  CHECK(info.storageSize == HEADER_SIZE + 4u * (SLOT_HDR_SIZE + 24u));

  ringFileClose(h);

  h = mustOpenMeter("resize", false);
  CHECK(infoCount(h) == 3);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 2)));

  //-- Grow back and keep using the ring.
  CHECK_ERR(ringFileResize(h, 6), ESP_OK);
  CHECK(infoCount(h) == 3);
  CHECK_ERR(addHour(h, 0, 5, 50), ESP_OK);
  CHECK(infoCount(h) == 4);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 5)));

  CHECK_ERR(ringFileResize(h, 6), ESP_OK);
  CHECK_ERR(ringFileResize(h, 0), ESP_ERR_INVALID_ARG);
  ringFileClose(h);
}

static void testResizeInterruptedStates(void)
{
  for (int scenario = 0; scenario < 5; scenario++)
  {
    char name[32];
    snprintf(name, sizeof(name), "resizeInt%d", scenario);
    ringFileHandle_t *h = mustCreateMeter(name, 5);

    for (uint32_t hh = 0; hh < 3; hh++)
    {
      CHECK_ERR(addHour(h, 0, hh, 10u + hh), ESP_OK);
    }

    installHooks();

    esp_err_t err;

    switch (scenario)
    {
      case 0:
        failNext(OP_WRITE, 1);
        break;

      case 1:
        failNext(OP_FSYNC, 1);
        break;

      case 2:
        failNextOn(OP_RENAME, 1, ".rfsold");
        break;

      case 3:
        failNextOn(OP_RENAME, 1, ".rfsnew");
        break;

      default:
        failErrno(ENOSPC);
        failNextOn(OP_OPEN, 1, ".rfsnew");
        break;
    }

    err = ringFileResize(h, 3);
    logCmdText(err == ESP_OK ? "ESP_OK (injection did not hit)" : "rejected",
               "resize 5->3 with injected failure (scenario %d)", scenario);
    CHECK(err != ESP_OK);
    ringFileClose(h);

    h = mustOpenMeter(name, false);
    CHECK(infoCount(h) == 3);
    meterFull_t full;
    memset(&full, 0, sizeof(full));
    CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
    CHECK(full.keys.usedKey == hourKey(hourTs(0, 0)));
    ringFileClose(h);
  }

  //-- A leftover candidate file is removed on the next read-write open.
  ringFileHandle_t *h = mustCreateMeter("resizeStray", 3);
  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  ringFileClose(h);

  char path[300];
  char stray[340];
  ringPath(path, sizeof(path), "resizeStray");
  snprintf(stray, sizeof(stray), "%s.rfsnew", path);
  FILE *f = fopen(stray, "wb");

  if (f != NULL)
  {
    fwrite("junk", 1, 4, f);
    fclose(f);
  }

  CHECK(pathExists(stray));
  h = mustOpenMeter("resizeStray", false);
  CHECK(!pathExists(stray));
  CHECK(infoCount(h) == 1);
  ringFileClose(h);
}

static void testFailureInjectionMutations(void)
{
  ringFileHandle_t *h = mustCreateMeter("failmut", 3);
  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);

  installHooks();
  failNext(OP_WRITE, 1);
  CHECK_ERR(addHour(h, 0, 1, 20), RING_FILE_STORE_ERR_IO);

  //-- The handle is quarantined until a reopen runs recovery.
  meterRec_t rec;
  mkMeter(&rec, 1, 0);
  CHECK_ERR(ringFileAdd(h, hourTs(0, 2), &rec),
            RING_FILE_STORE_ERR_IO_FAULTED);
  ringFileClose(h);

  h = mustOpenMeter("failmut", false);
  CHECK(infoCount(h) == 1);
  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 0)));
  ringFileClose(h);

  //-- Write failure on a buffered flush: the staged records never reach
  //-- storage, the committed record survives.
  h = mustCreateMeter("failmut2", 3);
  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  ringFileClose(h);

  h = mustOpenMeterMode("failmut2", false, RING_FILE_PERSIST_BUFFERED, 8);
  CHECK_ERR(addHour(h, 0, 1, 20), ESP_OK);
  CHECK_ERR(addHour(h, 0, 2, 30), ESP_OK);
  installHooks();
  failNext(OP_WRITE, 1);
  CHECK_ERR(ringFileSync(h), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);

  h = mustOpenMeter("failmut2", false);
  CHECK(infoCount(h) == 1);
  ringFileClose(h);
}

static void testUnchangedWrites(void)
{
  ringFileHandle_t *h = mustCreateMeter("unchanged", 3);
  CHECK_ERR(addHour(h, 0, 1, 100), ESP_OK);

  ringFileStoreResetIoStats();

  meterRec_t same;
  mkMeter(&same, 100, 1);
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 1)), hourTs(0, 1), &same),
            ESP_OK);
  CHECK(lastStats().writes == 0);

  meterRec_t editSame;
  mkMeter(&editSame, 100, 1);
  CHECK_ERR(ringFileEdit(h, hourTs(0, 1), &editSame), ESP_OK);
  CHECK(lastStats().writes == 0);

  meterRec_t upsertSame;
  mkMeter(&upsertSame, 100, 1);
  CHECK_ERR(ringFileUpsert(h, hourTs(0, 1), &upsertSame), ESP_OK);
  CHECK(lastStats().writes == 0);

  meterRec_t changed;
  mkMeter(&changed, 101, 1);
  CHECK_ERR(ringFileUpdateByKey(h, hourKey(hourTs(0, 1)), hourTs(0, 1),
                                &changed), ESP_OK);
  CHECK(lastStats().writes == 1);

  ringFileClose(h);
}

static void testWearDistribution(void)
{
  ringFileHandle_t *h = mustCreateMeter("wear", 4);
  ringFileStoreResetIoStats();

  for (uint32_t hh = 0; hh < 8; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u * hh), ESP_OK);
  }

  rfsIoStats_t stats = lastStats();
  CHECK(stats.writes == 8);
  CHECK(infoCount(h) == 4);
  ringFileClose(h);

  char path[300];
  ringPath(path, sizeof(path), "wear");
  ringFileValidation_t val;
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.headerValid);
  CHECK(val.badSlotCount == 0);
  CHECK(val.committedCount == 4);
}

static void testBufferedMode(void)
{
  ringFileHandle_t *h = mustCreateMeter("buffered", 4);
  ringFileClose(h);

  h = mustOpenMeterMode("buffered", false, RING_FILE_PERSIST_BUFFERED, 8);

  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  CHECK_ERR(addHour(h, 0, 1, 20), ESP_OK);
  CHECK_ERR(addHour(h, 0, 2, 30), ESP_OK);

  ringFileInfo_t info;
  memset(&info, 0, sizeof(info));
  CHECK_ERR(ringFileGetInfo(h, &info), ESP_OK);
  CHECK(info.count == 3);
  CHECK(info.pendingCount == 3);
  CHECK(info.dirty);

  //-- Staged records report the staged marker, not a bucketId.
  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  CHECK(full.keys.usedKey == hourKey(hourTs(0, 0)));
  uint32_t slot = 0;
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
  CHECK(slot == RING_FILE_SLOT_STAGED);

  logRingDump(h, "buffered 3 staged");

  //-- Nothing reached storage yet: a second read-only handle sees nothing.
  ringFileHandle_t *ro = mustOpenMeter("buffered", true);
  CHECK(infoCount(ro) == 0);
  ringFileClose(ro);

  ringFileStoreResetIoStats();
  CHECK_ERR(ringFileSync(h), ESP_OK);
  CHECK(lastStats().writes >= 3);

  memset(&info, 0, sizeof(info));
  CHECK_ERR(ringFileGetInfo(h, &info), ESP_OK);
  CHECK(info.pendingCount == 0);
  CHECK(!info.dirty);

  //-- After the sync the bucketId is the period modulo capacity.
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
  CHECK(slot != RING_FILE_SLOT_STAGED);
  CHECK(slot == (uint32_t)((DAYS_2026_10_23 * 24ULL + 0ULL) % 4u));

  ringFileClose(h);

  h = mustOpenMeter("buffered", false);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);
}

static void testBufferedOverflowFlushes(void)
{
  ringFileHandle_t *h = mustCreateMeter("bufover", 8);
  ringFileClose(h);

  h = mustOpenMeterMode("bufover", false, RING_FILE_PERSIST_BUFFERED, 2);
  ringFileStoreResetIoStats();

  CHECK_ERR(addHour(h, 0, 0, 10), ESP_OK);
  CHECK_ERR(addHour(h, 0, 1, 20), ESP_OK);
  CHECK(lastStats().writes == 0);

  //-- The third add overflows the two-entry buffer and commits.
  CHECK_ERR(addHour(h, 0, 2, 30), ESP_OK);
  CHECK(lastStats().writes >= 2);

  CHECK(infoCount(h) == 3);
  ringFileClose(h);

  h = mustOpenMeter("bufover", false);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);
}

static void testMultipleTypes(void)
{
  ringFileHandle_t *meter = mustCreateMeter("typesMeter", 5);
  ringFileConfig_t mcfg = monthConfig(12);
  ringFileHandle_t *month = mustCreateCfg("typesMonth", &mcfg);
  ringFileConfig_t scfg = sensorConfig(32);
  ringFileHandle_t *sensor = mustCreateCfg("typesSensor", &scfg);

  for (uint32_t hh = 0; hh < 5; hh++)
  {
    CHECK_ERR(addHour(meter, 0, hh, 100u + hh), ESP_OK);
  }

  ringFileStoreResetIoStats();
  monthRec_t mrec;
  mkMonth(&mrec, 1000, 1, 3000);
  CHECK_ERR(ringFileAdd(month, 20260900000000ULL, &mrec), ESP_OK);
  mrec.totalWh = 2000;
  CHECK_ERR(ringFileAdd(month, 20261000000000ULL, &mrec), ESP_OK);
  mrec.totalWh = 3000;
  CHECK_ERR(ringFileAdd(month, 20261100000000ULL, &mrec), ESP_OK);

  sensorRec_t srec;
  mkSensor(&srec, 2150, 40);
  CHECK_ERR(ringFileAdd(sensor, 20261118143055ULL, &srec), ESP_OK);
  srec.tempMilliC = 2200;
  CHECK_ERR(ringFileAdd(sensor, 20261118143056ULL, &srec), ESP_OK);
  srec.tempMilliC = 2250;
  CHECK_ERR(ringFileAdd(sensor, 20261118143057ULL, &srec), ESP_OK);

  ringFileInfo_t info;

  memset(&info, 0, sizeof(info));
  CHECK_ERR(ringFileGetInfo(month, &info), ESP_OK);
  CHECK(strcmp(info.schemaId, RING_FILE_SCHEMA_YYMM) == 0);
  CHECK(info.granularity == RING_FILE_GRANULARITY_MONTH);
  CHECK(info.recordSize == 16u + sizeof(monthRec_t));
  CHECK(info.count == 3);

  memset(&info, 0, sizeof(info));
  CHECK_ERR(ringFileGetInfo(sensor, &info), ESP_OK);
  CHECK(strcmp(info.schemaId, RING_FILE_SCHEMA_YYMMDDHHMMSS) == 0);
  CHECK(info.granularity == RING_FILE_GRANULARITY_SECOND);
  CHECK(info.recordSize == 16u + sizeof(sensorRec_t));
  CHECK(info.count == 3);

  //-- The October 2026 month bucket is 24310 % 12.
  monthFull_t mfull;
  memset(&mfull, 0, sizeof(mfull));
  CHECK_ERR(ringFileReadByKey(month, 202610ULL, &mfull), ESP_OK);
  CHECK(mfull.keys.timeStamp == 20261000000000ULL);
  CHECK(mfull.payload.totalWh == 2000);

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  CHECK_ERR(ringFileIteratorBegin(month, &it, RING_FILE_ITER_KEY,
                                  &(uint64_t){202610ULL}, &mfull), ESP_OK);
  uint32_t slot = 0;
  CHECK_ERR(ringFileIteratorGetSlot(month, &it, &slot), ESP_OK);
  CHECK(slot == 24310u % 12u);

  //-- Per-second ring: consecutive seconds get consecutive buckets.
  sensorFull_t sfull;
  memset(&sfull, 0, sizeof(sfull));
  CHECK_ERR(ringFileReadByKey(sensor, 20261118143056ULL, &sfull), ESP_OK);
  CHECK(sfull.payload.tempMilliC == 2200);

  memset(&it, 0, sizeof(it));
  memset(&sfull, 0, sizeof(sfull));
  CHECK_ERR(ringFileIteratorBegin(sensor, &it, RING_FILE_ITER_OLDEST, NULL,
                                  &sfull), ESP_OK);
  CHECK_ERR(ringFileIteratorGetSlot(sensor, &it, &slot), ESP_OK);
  CHECK(slot == (uint32_t)(1795012255ULL % 32u));

  ringFileClose(meter);
  ringFileClose(month);
  ringFileClose(sensor);

  //-- Opening one type with the other's expectation fails.
  char path[300];
  ringPath(path, sizeof(path), "typesMonth");
  ringFileHandle_t *o = NULL;
  ringFileOpenConfig_t wrong = baseOpen(RING_FILE_SCHEMA_YYMMDDHH,
                                        sizeof(meterRec_t), false,
                                        RING_FILE_PERSIST_IMMEDIATE, 0);
  CHECK_ERR(ringFileOpen(path, &wrong, &o), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  ringFileOpenConfig_t right = baseOpen(RING_FILE_SCHEMA_YYMM,
                                        sizeof(monthRec_t), false,
                                        RING_FILE_PERSIST_IMMEDIATE, 0);
  CHECK_ERR(ringFileOpen(path, &right, &o), ESP_OK);
  CHECK(infoCount(o) == 3);
  ringFileClose(o);
}

static void testValidateGoodRing(void)
{
  ringFileHandle_t *h = mustCreateMeter("validate", 3);

  for (uint32_t hh = 0; hh < 3; hh++)
  {
    CHECK_ERR(addHour(h, 0, hh, 10u + hh), ESP_OK);
  }

  ringFileClose(h);

  char path[300];
  ringPath(path, sizeof(path), "validate");
  ringFileValidation_t val;
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.headerValid);
  CHECK(val.metadataIssue == RING_FILE_META_OK);
  CHECK(val.capacity == 3);
  CHECK(val.recordSize == 24);
  CHECK(val.slotCount == 4);
  CHECK(val.badSlotCount == 0);
  CHECK(val.committedCount == 3);
  CHECK(!val.strayResizeFiles);

  uint8_t flip = 0xA5u;
  pokeFile(path, slotRecordOffset(0, 24) + 20, &flip, 1);
  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.badSlotCount == 1);
  CHECK(val.committedCount == 2);

  char stray[340];
  snprintf(stray, sizeof(stray), "%s.rfsold", path);
  FILE *f = fopen(stray, "wb");

  if (f != NULL)
  {
    fclose(f);
  }

  CHECK_ERR(ringFileValidate(path, &val), ESP_OK);
  CHECK(val.strayResizeFiles);
  unlink(stray);

  CHECK_ERR(ringFileValidate("/nonexistent/path.rfs", &val), ESP_ERR_NOT_FOUND);
}

static void testManyRecords(void)
{
  ringFileHandle_t *h = mustCreateMeter("many", 16);

  //-- 40 hours: days 23..26 of October 2026, hours 0..9 of each day.
  for (uint32_t d = 0; d < 4; d++)
  {
    for (uint32_t hh = 0; hh < 10; hh++)
    {
      CHECK_ERR(addHour(h, d, hh, 1000u + d * 10u + hh), ESP_OK);
    }
  }

  CHECK(infoCount(h) == 16);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadOldest(h, &full), ESP_OK);
  CHECK(full.keys.timeStamp == mkTs(2026u, 10u, 25u, 4u, 0u, 0u));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.timeStamp == mkTs(2026u, 10u, 26u, 9u, 0u, 0u));

  //-- Walk the ring: the retained 16 buckets are the newest ones, oldest
  //-- to newest: day 25 hours 4..9, then day 26 hours 0..9.
  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);

  uint32_t seen = 0;

  for (;;)
  {
    uint32_t dOff = (seen < 6u) ? 2u : 3u;
    uint32_t hh = (seen < 6u) ? (4u + seen) : (seen - 6u);
    CHECK(full.keys.timeStamp == mkTs(2026u, 10u, 23u + dOff, hh, 0u, 0u));
    CHECK(full.keys.usedKey == hourKey(mkTs(2026u, 10u, 23u + dOff, hh, 0u, 0u)));
    seen++;

    if (ringFileIteratorNext(h, &it, &full) != ESP_OK)
    {
      break;
    }
  }

  CHECK(seen == 16);
  ringFileClose(h);
}

//-- The slot-directory cache: committed-state queries run from RAM after
//-- one scan per disk epoch. Guards against a regression back to the
//-- per-query full-slot rescans (which cost >10^4 reads at this size).
static void testDirectoryCache(void)
{
  ringFileHandle_t *h = mustCreateMeter("cachedir", 16);

  for (uint32_t d = 0; d < 4; d++)
  {
    for (uint32_t hh = 0; hh < 10; hh++)
    {
      CHECK_ERR(addHour(h, d, hh, 2000u + d * 10u + hh), ESP_OK);
    }
  }

  CHECK(infoCount(h) == 16);

  ringFileStoreResetIoStats();

  meterFull_t full;
  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);

  uint32_t seen = 1;

  while (ringFileIteratorNext(h, &it, &full) == ESP_OK)
  {
    seen++;
  }

  CHECK(seen == 16);

  rfsIoStats_t stats = lastStats();

  //-- Directory build (2 reads x 17 slots) plus one record load per
  //-- returned record (2 reads x 16) ≈ 66 reads. The uncached scan cost
  //-- several thousand reads at this size.
  CHECK(stats.reads > 0);
  CHECK(stats.reads < 300);

  ringFileStoreResetIoStats();

  //-- A keyed read on the warm cache is a record load only.
  uint64_t probeKey = hourKey(hourTs(3u, 9u));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadByKey(h, probeKey, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(3u, 9u));
  stats = lastStats();
  CHECK(stats.reads <= 4);

  ringFileStoreResetIoStats();

  //-- A write through the handle invalidates the cache: the next query
  //-- rescans (>= 34 reads) and sees the new newest record.
  CHECK_ERR(addHour(h, 3u, 10u, 2333u), ESP_OK);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(h, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(3u, 10u));
  stats = lastStats();
  CHECK(stats.reads >= 30);
  CHECK(stats.reads < 300);
  CHECK(infoCount(h) == 16);

  ringFileClose(h);
}

//-- Cache semantics across handles: a handle serves queries from its own
//-- cache and sees its own writes immediately; commits by another handle
//-- become visible after ringFileCacheInvalidate().
static void testCrossHandleCache(void)
{
  ringFileHandle_t *a = mustCreateMeter("cachestale", 4);
  CHECK_ERR(addHour(a, 0u, 0u, 11u), ESP_OK);

  ringFileHandle_t *b = mustOpenMeter("cachestale", true);
  CHECK(infoCount(b) == 1);

  meterFull_t full;
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(b, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0u, 0u));

  //-- Write through handle a: handle b keeps its cached view.
  CHECK_ERR(addHour(a, 0u, 1u, 12u), ESP_OK);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(b, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0u, 0u));

  //-- a always sees its own commit.
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(a, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0u, 1u));

  //-- Explicit invalidation lets b observe the other handle's commit.
  CHECK_ERR(ringFileCacheInvalidate(b), ESP_OK);
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileReadNewest(b, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0u, 1u));

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorBegin(b, &it, RING_FILE_ITER_OLDEST, NULL, &full),
            ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0u, 0u));
  memset(&full, 0, sizeof(full));
  CHECK_ERR(ringFileIteratorNext(b, &it, &full), ESP_OK);
  CHECK(full.keys.timeStamp == hourTs(0u, 1u));
  CHECK(ringFileIteratorNext(b, &it, &full) == RING_FILE_STORE_ERR_ITER_END);

  ringFileClose(b);
  ringFileClose(a);
}

static void testTimeHelpers(void)
{
  char text[15];
  uint64_t unixSec = 0;

  CHECK_ERR(ringFileUnixToTimeStampString(946684800ULL, text), ESP_OK);
  CHECK(strcmp(text, "20000101000000") == 0);

  CHECK_ERR(ringFileTimeStampStringToUnix("20261118143055", &unixSec), ESP_OK);
  CHECK(unixSec == 1795012255ULL);

  CHECK_ERR(ringFileUnixToTimeStampString(1795012255ULL, text), ESP_OK);
  CHECK(strcmp(text, "20261118143055") == 0);

  CHECK_ERR(ringFileTimeStampStringToUnix("19991231235959", &unixSec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("20260230000000", &unixSec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("20260101", &unixSec),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUnixToTimeStampString(0, text), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUnixToTimeStampString(4133980800ULL, text),
            ESP_ERR_INVALID_ARG);

  uint64_t period = 0;
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_YEAR,
                                   &period), ESP_OK);
  CHECK(period == 2000ULL);
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_MONTH,
                                   &period), ESP_OK);
  CHECK(period == (2000ULL - 1ULL) * 12ULL + 1ULL);
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_DAY,
                                   &period), ESP_OK);
  CHECK(period == 10957ULL);
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_HOUR,
                                   &period), ESP_OK);
  CHECK(period == 10957ULL * 24ULL);
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_MINUTE,
                                   &period), ESP_OK);
  CHECK(period == 10957ULL * 1440ULL);
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_SECOND,
                                   &period), ESP_OK);
  CHECK(period == 946684800ULL);
  CHECK_ERR(ringFileUnixToBucketId(946684800ULL, RING_FILE_GRANULARITY_NONE,
                                   &period), ESP_ERR_INVALID_ARG);

  uint32_t isoYear = 0;
  uint32_t isoWeek = 0;

  //-- 2025-12-29 is the Monday of ISO year 2026, week 1.
  CHECK_ERR(ringFileUnixToIsoWeek(1766966400ULL, &isoYear, &isoWeek), ESP_OK);
  CHECK(isoYear == 2026u);
  CHECK(isoWeek == 1u);

  //-- 2026-01-01 (Thursday) stays in 2026-W01.
  CHECK_ERR(ringFileUnixToIsoWeek(1767225600ULL, &isoYear, &isoWeek), ESP_OK);
  CHECK(isoYear == 2026u);
  CHECK(isoWeek == 1u);

  //-- A second-resolution period equals the unix seconds.
  CHECK_ERR(ringFileTimeStampToPeriod(20261118143055ULL,
                                      RING_FILE_GRANULARITY_SECOND, &period),
            ESP_OK);
  CHECK(period == 1795012255ULL);
}

int main(int argc, char *argv[])
{
  snprintf(sDir, sizeof(sDir), "/tmp/rfs_test_XXXXXX");

  if (mkdtemp(sDir) == NULL)
  {
    printf("cannot create temp dir\n");
    return 2;
  }

  installHooks();

  struct
  {
    const char *name;
    void (*fn)(void);
  } tests[] = {
    {"arguments", testArguments},
    {"createOpenReopen", testCreateOpenReopen},
    {"emptyRing", testEmptyRing},
    {"singleSlotRing", testSingleSlotRing},
    {"sameBucketUpdate", testSameBucketUpdate},
    {"notNewer", testNotNewer},
    {"timestampValidation", testTimestampValidation},
    {"bucketAndKeyDerivation", testBucketAndKeyDerivation},
    {"evictionOrder", testEvictionOrder},
    {"editWindow", testEditWindow},
    {"updateByKeyRules", testUpdateByKeyRules},
    {"iterator", testIterator},
    {"schemaMismatch", testSchemaMismatch},
    {"readOnly", testReadOnly},
    {"staleLockBreak", testStaleLockBreak},
    {"corruption", testCorruption},
    {"clear", testClear},
    {"resize", testResize},
    {"resizeInterruptedStates", testResizeInterruptedStates},
    {"failureInjectionMutations", testFailureInjectionMutations},
    {"unchangedWrites", testUnchangedWrites},
    {"wearDistribution", testWearDistribution},
    {"bufferedMode", testBufferedMode},
    {"bufferedOverflowFlushes", testBufferedOverflowFlushes},
    {"multipleTypes", testMultipleTypes},
    {"validateGoodRing", testValidateGoodRing},
    {"manyRecords", testManyRecords},
    {"directoryCache", testDirectoryCache},
    {"crossHandleCache", testCrossHandleCache},
    {"timeHelpers", testTimeHelpers},
  };

  //-- testLog.md: path from argv[1] so run_tests.sh can place it in the
  //-- repository root. The log records every logged command (with data),
  //-- its result, the key/bucket dumps and a per-test overview table.
  const char *logPath = (argc > 1) ? argv[1] : "testLog.md";
  gLog = fopen(logPath, "w");

  if (gLog == NULL)
  {
    printf("  WARN cannot open test log %s: %s\n", logPath, strerror(errno));
  }
  else
  {
    char stamp[64];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%d-%m-%Y - %H:%M", localtime(&now));
    logLine("# ringFileStore — test log (host suite)");
    logLine("");
    logLine("Generated: %s — host tests with POSIX shims and "
            "failure-injection.", stamp);
    logLine("");
    logLine("Every stored record carries the automatic key fields "
            "timeStamp and usedKey; ring dumps show both plus the reported "
            "bucketId (period %% capacity).");
    logLine("");
    logLine("| Test | checks | failures |");
    logLine("| --- | ---: | ---: |");
  }

  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
  {
    gTestName = tests[i].name;
    installHooks();
    int checksBefore = gChecks;
    int failuresBefore = gFailures;
    logLine("");
    logLine("## Test: %s", tests[i].name);
    tests[i].fn();
    logLine("- **Test result:** %d checks, %d new failures",
            gChecks - checksBefore, gFailures - failuresBefore);
    logLine("| %s | %d | %d |", tests[i].name, gChecks - checksBefore,
            gFailures - failuresBefore);
    printf("  %-28s %d checks, %d failures so far\n", tests[i].name, gChecks,
           gFailures);
  }

  logLine("");
  logLine("## Summary");
  logLine("");
  logLine("**%s**: %d checks, %d failures.", gFailures == 0 ? "PASS" : "FAIL",
          gChecks, gFailures);

  if (gLog != NULL)
  {
    fclose(gLog);
    gLog = NULL;
    printf("  test log written to %s\n", logPath);
  }

  printf("\n%s: %d checks, %d failures\n", gFailures == 0 ? "PASS" : "FAIL",
         gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
