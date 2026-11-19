//--
//-- FileVersion: [3] 19-11-2026 - 09:06
//--
//-- ringFileStore - host-side automated tests for the ring_file_store
//-- component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//--
//-- The component sources are compiled against small POSIX shims
//-- (shim/) so the whole suite runs on the dev machine. Storage failures
//-- are injected by replacing the backend syscall table (rfs_file_io.h)
//-- around every write, sync and rename boundary, then inspecting the
//-- state after reopening.
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
  if (consumeFailure(OP_RENAME, fromPath))
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

typedef struct
{
  uint64_t timestamp;
  uint32_t energyWh;
  uint16_t errorCount;
  uint16_t padding;
} meterRec_t;

typedef struct
{
  uint8_t raw[16];
  uint32_t value;
} keyedRec_t;

typedef struct
{
  uint8_t raw[80];
  uint32_t value;
} bigRec_t;

//-- ---------------------------------------------------------------------------
//-- testLog.md writer: one "Command" line (with its data) followed by its
//-- "Result", plus ring dumps showing key and physical slot. Path comes
//-- from argv[1] (run_tests.sh passes the repository root).
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
  meterRec_t rec;
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
      logLine("  - slot **staged** (not yet on storage) key=%llu "
              "energyWh=%lu errorCount=%u",
              (unsigned long long)rec.timestamp, (unsigned long)rec.energyWh,
              (unsigned)rec.errorCount);
    }
    else
    {
      logLine("  - slot %lu key=%llu energyWh=%lu errorCount=%u",
              (unsigned long)slot, (unsigned long long)rec.timestamp,
              (unsigned long)rec.energyWh, (unsigned)rec.errorCount);
    }

    if (ringFileIteratorNext(h, &it, &rec) != ESP_OK)
    {
      break;
    }
  }
}

static void ringPath(char *out, size_t size, const char *name)
{
  snprintf(out, size, "%s/%s.rfs", sDir, name);
}

static ringFileConfig_t meterConfig(uint32_t capacity)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = capacity;
  cfg.schemaId = 1;
  cfg.schemaVersion = 1;
  cfg.recordSize = sizeof(meterRec_t);
  cfg.key.keyOffset = (uint32_t)offsetof(meterRec_t, timestamp);
  cfg.key.keyType = RING_FILE_KEY_U64;
  cfg.key.keySize = (uint16_t)sizeof(uint64_t);
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileOpenConfig_t meterOpen(bool readOnly, ringFilePersistMode_t mode,
                                      uint16_t maxPending)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = 1;
  cfg.expectedSchemaVersion = 1;
  cfg.expectedRecordSize = sizeof(meterRec_t);
  cfg.readOnly = readOnly;
  cfg.persistMode = mode;
  cfg.maxPendingRecords = maxPending;
  return cfg;
}

static void mkMeter(meterRec_t *m, uint64_t ts, uint32_t wh, uint16_t errs)
{
  memset(m, 0, sizeof(*m));
  m->timestamp = ts;
  m->energyWh = wh;
  m->errorCount = errs;
}

static const uint64_t METER_KEY = 4242;

static ringFileHandle_t *mustCreate(const char *name, uint32_t capacity)
{
  char path[300];
  ringPath(path, sizeof(path), name);
  ringFileConfig_t cfg = meterConfig(capacity);
  ringFileHandle_t *h = NULL;
  esp_err_t err = ringFileCreate(path, &cfg, &h);

  if (err != ESP_OK)
  {
    printf("  FATAL create %s: %s\n", name, esp_err_to_name(err));
    exit(2);
  }

  return h;
}

static ringFileHandle_t *mustOpenMode(const char *name, bool readOnly,
                                      ringFilePersistMode_t mode,
                                      uint16_t maxPending)
{
  char path[300];
  ringPath(path, sizeof(path), name);
  ringFileOpenConfig_t cfg = meterOpen(readOnly, mode, maxPending);
  ringFileHandle_t *h = NULL;
  esp_err_t err = ringFileOpen(path, &cfg, &h);

  if (err != ESP_OK)
  {
    printf("  FATAL open %s: %s\n", name, esp_err_to_name(err));
    exit(2);
  }

  return h;
}

static ringFileHandle_t *mustOpen(const char *name, bool readOnly)
{
  return mustOpenMode(name, readOnly, RING_FILE_PERSIST_IMMEDIATE, 0);
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

#define SLOT_HDR_SIZE 20u
#define HEADER_SIZE 96u

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
  ringFileConfig_t cfg = meterConfig(4);
  ringFileHandle_t *h = NULL;
  char path[300];
  ringPath(path, sizeof(path), "args");

  CHECK_ERR(ringFileCreate(NULL, &cfg, &h), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileCreate(path, NULL, &h), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileCreate(path, &cfg, NULL), ESP_ERR_INVALID_ARG);

  cfg.maxRecords = 0;
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_ERR_INVALID_ARG);
  cfg = meterConfig(4);
  cfg.recordSize = 0;
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_ERR_INVALID_ARG);
  cfg = meterConfig(4);
  cfg.key.keySize = 4; //-- u64 key must carry 8 bytes
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_ERR_INVALID_ARG);
  cfg = meterConfig(4);
  cfg.key.keyOffset = (uint32_t)sizeof(meterRec_t) - 4; //-- key past the record
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_ERR_INVALID_ARG);

  h = mustCreate("args", 4);
  CHECK_ERR(ringFileAdd(NULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileAdd(h, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileReadByKey(h, NULL, NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileResize(h, 0), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileGetInfo(h, NULL), ESP_ERR_INVALID_ARG);
  ringFileClose(h);
}

static void testCreateOpenReopen(void)
{
  char path[300];
  ringPath(path, sizeof(path), "create");
  ringFileConfig_t cfg = meterConfig(4);
  ringFileHandle_t *h = NULL;
  ringFileHandle_t *h2 = NULL;

  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_OK);
  CHECK_ERR(ringFileCreate(path, &cfg, &h2), RING_FILE_STORE_ERR_ALREADY_EXISTS);
  CHECK(h != NULL);

  meterRec_t rec;
  mkMeter(&rec, 100, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  ringFileClose(h);

  h = mustOpen("create", false);
  CHECK(infoCount(h) == 1);
  ringFileClose(h);

  ringFileOpenConfig_t ocfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  CHECK_ERR(ringFileOpen("no_such_dir_missing/missing.rfs", &ocfg, &h),
            ESP_ERR_NOT_FOUND);
}

static void testEmptyRing(void)
{
  ringFileHandle_t *h = mustCreate("empty", 3);
  meterRec_t rec;
  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));

  CHECK(infoCount(h) == 0);
  CHECK_ERR(ringFileReadOldest(h, &rec), RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileReadNewest(h, &rec), RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &rec),
            RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_NEWEST, NULL, &rec),
            RING_FILE_STORE_ERR_RING_EMPTY);
  CHECK_ERR(ringFileIteratorNext(h, &it, &rec), RING_FILE_STORE_ERR_ITER_INVALIDATED);
  ringFileClose(h);
}

static void testSingleSlotRing(void)
{
  ringFileHandle_t *h = mustCreate("single", 1);
  meterRec_t rec, out;

  mkMeter(&rec, 10, 100, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK(infoCount(h) == 1);

  mkMeter(&rec, 20, 200, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK); //-- evicts key 10
  CHECK(infoCount(h) == 1);
  CHECK_ERR(ringFileReadByKey(h, &METER_KEY, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  uint64_t k = 10;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  k = 20;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 200);

  //-- Reuse the evicted key.
  mkMeter(&rec, 10, 101, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  k = 20;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  k = 10;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);

  //-- Repeated edits of one key on a one-record ring alternate between the
  //-- record slot and the scratch slot (bounded writes, no header rewrite).
  for (int i = 0; i < 100; i++)
  {
    mkMeter(&rec, 10, (uint32_t)i, 0);
    CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  }

  ringFileClose(h);
  h = mustOpen("single", false);
  CHECK(infoCount(h) == 1);
  k = 10;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 99);
  ringFileClose(h);
}

static void testDuplicatesAndWraparound(void)
{
  ringFileHandle_t *h = mustCreate("wrap", 3);
  meterRec_t rec, out;

  //-- Spec scenario: keys arrive 100, 300, 200 then 50.
  mkMeter(&rec, 100, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 300, 2, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 200, 3, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  uint64_t k = 100;
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_DUPLICATE_KEY);
  (void)k;
  mkMeter(&rec, 100, 9, 0);
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_DUPLICATE_KEY);

  mkMeter(&rec, 50, 4, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK); //-- evicts oldest (100)
  CHECK(infoCount(h) == 3);

  uint64_t key = 100;
  CHECK_ERR(ringFileReadByKey(h, &key, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  key = 300;
  CHECK_ERR(ringFileReadByKey(h, &key, &out), ESP_OK);
  CHECK(out.energyWh == 2);
  key = 50;
  CHECK_ERR(ringFileReadByKey(h, &key, &out), ESP_OK);
  CHECK(out.energyWh == 4);

  //-- Oldest is 300 and newest is 50.
  CHECK_ERR(ringFileReadOldest(h, &out), ESP_OK);
  CHECK(out.timestamp == 300);
  CHECK_ERR(ringFileReadNewest(h, &out), ESP_OK);
  CHECK(out.timestamp == 50);

  //-- Updating 200 preserves its position: order stays 300, 200, 50.
  key = 200;
  mkMeter(&rec, 200, 33, 0);
  CHECK_ERR(ringFileUpdateByKey(h, &key, &rec), ESP_OK);
  ringFileClose(h);

  h = mustOpen("wrap", false);
  ringFileIterator_t it;
  uint64_t order[3];
  int n = 0;
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &out),
            ESP_OK);
  order[n++] = out.timestamp;

  while (ringFileIteratorNext(h, &it, &out) == ESP_OK && n < 3)
  {
    order[n++] = out.timestamp;
  }

  CHECK(n == 3);
  CHECK(order[0] == 300 && order[1] == 200 && order[2] == 50);
  ringFileClose(h);
}

static void testUpdateSemantics(void)
{
  ringFileHandle_t *h = mustCreate("update", 4);
  meterRec_t rec, out;
  uint64_t k = 7;

  //-- Update of a key that is not active, with a matching embedded key.
  mkMeter(&rec, 7, 0, 0);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  mkMeter(&rec, 7, 10, 1);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

  //-- Replacement whose embedded key differs from the lookup key.
  mkMeter(&rec, 8, 11, 1);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_ERR_INVALID_ARG);

  //-- Lookup key without terminator (string type check is elsewhere); for
  //-- u64 all byte patterns are valid, so just verify normal update.
  mkMeter(&rec, 7, 12, 2);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 12 && out.errorCount == 2);

  //-- Upsert: existing key updates in place, new key appends.
  mkMeter(&rec, 7, 13, 0);
  CHECK_ERR(ringFileUpsert(h, &rec), ESP_OK);
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 13);
  mkMeter(&rec, 8, 14, 0);
  CHECK_ERR(ringFileUpsert(h, &rec), ESP_OK);
  CHECK(infoCount(h) == 2);
  CHECK_ERR(ringFileReadNewest(h, &out), ESP_OK);
  CHECK(out.timestamp == 8);

  ringFileClose(h);
}

static void testKeyTypes(void)
{
  static const ringFileKeyType_t intTypes[] = {
    RING_FILE_KEY_S8, RING_FILE_KEY_U8, RING_FILE_KEY_S16, RING_FILE_KEY_U16,
    RING_FILE_KEY_S32, RING_FILE_KEY_U32, RING_FILE_KEY_S64, RING_FILE_KEY_U64
  };
  static const uint16_t intWidths[] = {1, 1, 2, 2, 4, 4, 8, 8};

  for (size_t t = 0; t < sizeof(intTypes) / sizeof(intTypes[0]); t++)
  {
    char name[64];
    snprintf(name, sizeof(name), "keyint%zu", t);
    char path[300];
    ringPath(path, sizeof(path), name);

    ringFileConfig_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.maxRecords = 3;
    cfg.schemaId = 2;
    cfg.schemaVersion = 1;
    cfg.recordSize = sizeof(keyedRec_t);
    cfg.key.keyOffset = 0;
    cfg.key.keyType = intTypes[t];
    cfg.key.keySize = intWidths[t];

    ringFileHandle_t *h = NULL;
    CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_OK);

    keyedRec_t rec, out;
    memset(&rec, 0, sizeof(rec));
    rec.value = 1234;
    memset(rec.raw, 0xAB, sizeof(rec.raw));
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

    //-- Different value must not match, same value must match.
    uint8_t other[8] = {0};
    other[intWidths[t] - 1] = 0xFF;
    CHECK_ERR(ringFileReadByKey(h, other, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
    CHECK_ERR(ringFileReadByKey(h, rec.raw, &out), ESP_OK);
    CHECK(out.value == 1234);

    //-- Bytes after the key field are not part of the key: change them.
    memset(rec.raw + intWidths[t], 0x5A, sizeof(rec.raw) - intWidths[t]);
    CHECK_ERR(ringFileReadByKey(h, other, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);

    ringFileClose(h);
  }

  //-- Byte-array key: exact byte equality.
  char path[300];
  ringPath(path, sizeof(path), "keybytes");
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 3;
  cfg.schemaId = 2;
  cfg.schemaVersion = 1;
  cfg.recordSize = sizeof(keyedRec_t);
  cfg.key.keyOffset = 0;
  cfg.key.keyType = RING_FILE_KEY_BYTES;
  cfg.key.keySize = 6;
  ringFileHandle_t *h = NULL;
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_OK);
  keyedRec_t rec, out;
  memset(&rec, 0, sizeof(rec));
  memcpy(rec.raw, "\x01\x02\x03\x04\x05\x06", 6);
  rec.value = 1;
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK_ERR(ringFileReadByKey(h, rec.raw, &out), ESP_OK);
  rec.raw[5] = 0x07;
  CHECK_ERR(ringFileReadByKey(h, rec.raw, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  ringFileClose(h);

  //-- String key: case sensitive, bytes after the terminator ignored,
  //-- unterminated rejected.
  ringPath(path, sizeof(path), "keystr");
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 3;
  cfg.schemaId = 2;
  cfg.schemaVersion = 1;
  cfg.recordSize = sizeof(keyedRec_t);
  cfg.key.keyOffset = 0;
  cfg.key.keyType = RING_FILE_KEY_STRING;
  cfg.key.keySize = 16;
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_OK);
  memset(&rec, 0, sizeof(rec));
  strcpy((char *)rec.raw, "hourly");
  rec.value = 1;
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

  uint8_t padded[16];
  memset(padded, 0, sizeof(padded));
  strcpy((char *)padded, "hourly");
  memset(padded + 7, 0xEE, 9); //-- bytes after the terminator
  CHECK_ERR(ringFileReadByKey(h, padded, &out), ESP_OK);

  uint8_t wrongCase[16];
  memset(wrongCase, 0, sizeof(wrongCase));
  strcpy((char *)wrongCase, "Hourly");
  CHECK_ERR(ringFileReadByKey(h, wrongCase, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  uint8_t unterminated[16];
  memset(unterminated, 'x', sizeof(unterminated));
  CHECK_ERR(ringFileReadByKey(h, unterminated, &out), ESP_ERR_INVALID_ARG);

  //-- Add a record whose embedded string key is unterminated: rejected.
  keyedRec_t badRec;
  memset(&badRec, 0, sizeof(badRec));
  memset(badRec.raw, 'y', sizeof(badRec.raw));
  CHECK_ERR(ringFileAdd(h, &badRec), ESP_ERR_INVALID_ARG);
  ringFileClose(h);
}

static void testIterator(void)
{
  ringFileHandle_t *h = mustCreate("iter", 4);
  meterRec_t rec, out;

  for (uint64_t ts = 1; ts <= 4; ts++)
  {
    mkMeter(&rec, ts * 10, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &out),
            ESP_OK);
  CHECK(out.timestamp == 10);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 20);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 30);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 40);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), RING_FILE_STORE_ERR_ITER_END);

  //-- Previous walks back through everything including the start record.
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 30);
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 20);
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 10);
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &out), RING_FILE_STORE_ERR_ITER_END);

  //-- Newest start.
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_NEWEST, NULL, &out),
            ESP_OK);
  CHECK(out.timestamp == 40);
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 30);

  //-- Exact key start, then both directions.
  uint64_t k = 30;
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY, &k, &out), ESP_OK);
  CHECK(out.timestamp == 30);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), ESP_OK);
  CHECK(out.timestamp == 40);
  uint64_t missing = 999;
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY, &missing, &out),
            RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  //-- A successful mutation invalidates live cursors on that handle.
  //-- (Capacity 4: adding 50 evicts the oldest key 10.)
  mkMeter(&rec, 50, 5, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), RING_FILE_STORE_ERR_ITER_INVALIDATED);
  CHECK_ERR(ringFileIteratorPrevious(h, &it, &out), RING_FILE_STORE_ERR_ITER_INVALIDATED);

  //-- Updating a record also invalidates cursors.
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &out),
            ESP_OK);
  CHECK(out.timestamp == 20);
  k = 30;
  mkMeter(&rec, 30, 11, 0);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), RING_FILE_STORE_ERR_ITER_INVALIDATED);

  //-- Unchanged update is not a mutation: cursor survives.
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &out),
            ESP_OK);
  ringFileReadByKey(h, &k, &out);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &out), ESP_OK);
  CHECK_ERR(ringFileIteratorNext(h, &it, &out), ESP_OK);

  ringFileClose(h);
}

static void testSchemaMismatch(void)
{
  char path[300];
  ringPath(path, sizeof(path), "schema");
  ringFileConfig_t cfg = meterConfig(4);
  cfg.schemaId = 7;
  cfg.schemaVersion = 3;
  ringFileHandle_t *h = NULL;
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_OK);
  ringFileClose(h);

  ringFileOpenConfig_t ocfg;
  memset(&ocfg, 0, sizeof(ocfg));
  ocfg.expectedSchemaId = 8;
  ocfg.expectedSchemaVersion = 3;
  ocfg.expectedRecordSize = sizeof(meterRec_t);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  ocfg.expectedSchemaId = 7;
  ocfg.expectedSchemaVersion = 4;
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  ocfg.expectedSchemaVersion = 3;
  ocfg.expectedRecordSize = sizeof(meterRec_t) + 4;
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  ocfg.expectedRecordSize = sizeof(meterRec_t);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), ESP_OK);
  ringFileClose(h);
}

static void testReadOnly(void)
{
  ringFileHandle_t *h = mustCreate("ro", 4);
  meterRec_t rec;

  mkMeter(&rec, 1, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  ringFileClose(h);

  ringFileStoreResetIoStats();
  h = mustOpen("ro", true);
  meterRec_t out;
  uint64_t k = 1;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileUpsert(h, &rec), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileClear(h), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileResize(h, 2), RING_FILE_STORE_ERR_READONLY);
  CHECK_ERR(ringFileSync(h), ESP_OK);
  ringFileClose(h);
  rfsIoStats_t stats = lastStats();
  CHECK(stats.writes == 0);
  CHECK(stats.fsyncs == 0);
  CHECK(stats.renames == 0);
  CHECK(stats.unlinks == 0);

  //-- A writable handle holds the exclusive lock; read-only handles coexist
  //-- and a second writable open is refused until the lock is released.
  ringFileHandle_t *w0 = mustOpen("ro", false);
  ringFileHandle_t *r1 = mustOpen("ro", true);
  ringFileHandle_t *r2 = mustOpen("ro", true);
  CHECK(r1 != NULL && r2 != NULL);
  ringFileHandle_t *w = NULL;
  ringFileOpenConfig_t ocfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  char path[300];
  ringPath(path, sizeof(path), "ro");
  CHECK_ERR(ringFileOpen(path, &ocfg, &w), RING_FILE_STORE_ERR_BUSY);
  ringFileClose(r1);
  ringFileClose(r2);
  CHECK_ERR(ringFileOpen(path, &ocfg, &w), RING_FILE_STORE_ERR_BUSY);
  ringFileClose(w0);

  //-- After the lock holder closes, the writable open succeeds again.
  CHECK_ERR(ringFileOpen(path, &ocfg, &w), ESP_OK);
  ringFileClose(w);
}

static void testStaleLockBreak(void)
{
  ringFileHandle_t *h = mustCreate("stale", 4);
  char path[300];
  ringPath(path, sizeof(path), "stale");
  meterRec_t rec;

  mkMeter(&rec, 1, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

  //-- A run that died before close leaves the lock behind and blocks every
  //-- later writable open; only after a reset (no live handle) may it be
  //-- broken, which is the sequence exercised here.
  ringFileOpenConfig_t ocfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  ringFileHandle_t *w = NULL;
  CHECK_ERR(ringFileOpen(path, &ocfg, &w), RING_FILE_STORE_ERR_BUSY);

  CHECK_ERR(ringFileBreakStaleLock(NULL), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileBreakStaleLock(path), ESP_OK);
  CHECK_ERR(ringFileBreakStaleLock(path), ESP_ERR_NOT_FOUND);

  //-- The abandoned handle closes with no lock file left; its release unlink
  //-- is a harmless no-op and must not fail.
  ringFileClose(h);

  //-- After the takeover the writable open succeeds and sees the data.
  CHECK_ERR(ringFileOpen(path, &ocfg, &w), ESP_OK);
  meterRec_t out;
  uint64_t k = 1;
  CHECK_ERR(ringFileReadByKey(w, &k, &out), ESP_OK);
  CHECK(out.energyWh == 1);
  ringFileClose(w);
}

static void testCorruption(void)
{
  char path[300];
  ringPath(path, sizeof(path), "corrupt");
  ringFileHandle_t *h = mustCreate("corrupt", 4);
  meterRec_t rec;

  for (uint64_t ts = 1; ts <= 3; ts++)
  {
    mkMeter(&rec, ts, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  ringFileClose(h);

  //-- Flip a byte inside record 1 (slot 1): that record fails its CRC and
  //-- disappears; the other records survive.
  uint8_t flip = 0xFF;
  pokeFile(path, slotRecordOffset(1, sizeof(meterRec_t)) + 8, &flip, 1);
  h = mustOpen("corrupt", false);
  uint64_t k = 1;
  meterRec_t out;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  k = 2;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  k = 3;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(infoCount(h) == 2);
  ringFileClose(h);

  ringFileValidation_t v;
  CHECK_ERR(ringFileValidate(path, &v), ESP_OK);
  CHECK(v.headerValid);
  CHECK(v.metadataIssue == RING_FILE_META_OK);
  CHECK(v.badSlotCount == 1);
  CHECK(v.committedCount == 2);
  CHECK(!v.strayResizeFiles);

  //-- A damaged header is reported as a checksum violation.

  //-- Bad magic.
  uint8_t magic[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  pokeFile(path, 0, magic, 4);
  CHECK_ERR(ringFileValidate(path, &v), ESP_OK);
  CHECK(!v.headerValid);
  CHECK(v.metadataIssue == RING_FILE_META_BAD_MAGIC);
  ringFileOpenConfig_t ocfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_FORMAT);

  //-- Truncated header.
  truncate(path, 40);
  CHECK_ERR(ringFileValidate(path, &v), ESP_OK);
  CHECK(!v.headerValid);
  CHECK(v.metadataIssue == RING_FILE_META_SHORT_FILE);

  //-- Extra trailing bytes: geometry violation.
  h = mustCreate("corrupt2", 3);
  ringFileClose(h);
  ringPath(path, sizeof(path), "corrupt2");
  int fd = open(path, O_WRONLY | O_APPEND);
  uint8_t junk[300]; //-- beyond header + (capacity + 1) slot bytes
  memset(junk, 0x77, sizeof(junk));
  CHECK(write(fd, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
  close(fd);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_CORRUPT);
  CHECK_ERR(ringFileValidate(path, &v), ESP_OK);
  CHECK(!v.headerValid);
  CHECK(v.metadataIssue == RING_FILE_META_BAD_GEOMETRY);

  //-- Missing file.
  CHECK_ERR(ringFileValidate("definitely_missing_file.rfs", &v),
            ESP_ERR_NOT_FOUND);
}

static void testClear(void)
{
  ringFileHandle_t *h = mustCreate("clear", 4);
  meterRec_t rec, out;

  for (uint64_t ts = 1; ts <= 3; ts++)
  {
    mkMeter(&rec, ts, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  CHECK(infoCount(h) == 3);
  CHECK_ERR(ringFileClear(h), ESP_OK);
  CHECK(infoCount(h) == 0);
  CHECK_ERR(ringFileReadOldest(h, &out), RING_FILE_STORE_ERR_RING_EMPTY);
  uint64_t k = 1;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);

  //-- Re-adding works after clear, keys reusable, capacity retained.
  mkMeter(&rec, 1, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK(infoCount(h) == 1);

  ringFileInfo_t info;
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.capacity == 4);
  CHECK(info.schemaId == 1);
  CHECK(info.schemaVersion == 1);
  ringFileClose(h);

  h = mustOpen("clear", false);
  CHECK(infoCount(h) == 1);
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  ringFileClose(h);
}

static void testResize(void)
{
  ringFileHandle_t *h = mustCreate("resize", 3);
  meterRec_t rec, out;

  mkMeter(&rec, 100, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 300, 2, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 200, 3, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 50, 4, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK); //-- wraps: 300,200,50

  ringFileInfo_t info;
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  uint64_t beforeSize = info.storageSize;

  //-- Shrink to two: retains the newest 200 and 50 (spec scenario).
  CHECK_ERR(ringFileResize(h, 2), ESP_OK);
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.capacity == 2);
  CHECK(info.storageSize < beforeSize);
  CHECK(info.count == 2);
  uint64_t k = 300;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  k = 200;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  k = 50;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK_ERR(ringFileReadOldest(h, &out), ESP_OK);
  CHECK(out.timestamp == 200);
  CHECK_ERR(ringFileReadNewest(h, &out), ESP_OK);
  CHECK(out.timestamp == 50);

  ringFileClose(h);
  h = mustOpen("resize", false);
  CHECK(infoCount(h) == 2);
  ringFileClose(h);

  //-- Grow back to 5 and verify insertion order of retained records.
  h = mustOpen("resize", false);
  CHECK_ERR(ringFileResize(h, 5), ESP_OK);
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.capacity == 5);
  CHECK(info.storageSize > beforeSize / 2);
  mkMeter(&rec, 25, 5, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  ringFileIterator_t it;
  uint64_t order[3];
  int n = 0;
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &out),
            ESP_OK);
  order[n++] = out.timestamp;

  while (ringFileIteratorNext(h, &it, &out) == ESP_OK && n < 3)
  {
    order[n++] = out.timestamp;
  }

  CHECK(n == 3);
  CHECK(order[0] == 200 && order[1] == 50 && order[2] == 25);
  ringFileClose(h);

  //-- Resize to capacity 1 retains the newest only; zero is rejected.
  h = mustOpen("resize", false);
  CHECK_ERR(ringFileResize(h, 0), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileResize(h, 1), ESP_OK);
  CHECK(infoCount(h) == 1);
  CHECK_ERR(ringFileReadOldest(h, &out), ESP_OK);
  CHECK(out.timestamp == 25);
  ringFileClose(h);

  h = mustOpen("resize", false);
  CHECK(infoCount(h) == 1);
  ringFileClose(h);
}

static void testResizeEmptyAndErrors(void)
{
  ringFileHandle_t *h = mustCreate("resizeEmpty", 2);
  meterRec_t rec;

  mkMeter(&rec, 1, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK_ERR(ringFileClear(h), ESP_OK);
  CHECK_ERR(ringFileResize(h, 5), ESP_OK);
  ringFileInfo_t info;
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.capacity == 5 && info.count == 0);
  mkMeter(&rec, 2, 2, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK(infoCount(h) == 1);
  ringFileClose(h);

  //-- Volume full while creating the candidate: original preserved.
  h = mustOpen("resizeEmpty", false);
  installHooks();
  failErrno(ENOSPC);
  failNextOn(OP_OPEN, 1, ".rfsnew");
  CHECK_ERR(ringFileResize(h, 6), RING_FILE_STORE_ERR_STORAGE_FULL);
  //-- Handle stays usable: the committed file was never touched.
  CHECK_ERR(ringFileResize(h, 6), ESP_OK);
  ringFileClose(h);
  h = mustOpen("resizeEmpty", false);
  CHECK(infoCount(h) == 1);
  ringFileClose(h);
}

static void testResizeInterruptedStates(void)
{
  ringFileHandle_t *h = mustCreate("resizeInt", 3);
  meterRec_t rec, out;
  char path[300];
  char oldPath[320], newPath[320];

  for (uint64_t ts = 1; ts <= 3; ts++)
  {
    mkMeter(&rec, ts, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  //-- Candidate write failure: original intact, handle reports I/O.
  installHooks();
  failNext(OP_WRITE, 1);
  CHECK_ERR(ringFileResize(h, 5), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);
  h = mustOpen("resizeInt", false);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);

  //-- First rename fails: original still at path, candidate cleaned.
  h = mustOpen("resizeInt", false);
  installHooks();
  failNextOn(OP_RENAME, 1, "/resizeInt.rfs");
  CHECK_ERR(ringFileResize(h, 5), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);
  h = mustOpen("resizeInt", false);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);

  //-- Second rename fails and the rollback rename also fails: both renames
  //-- fail via patterns. The handle quarantines; reopening recovers the
  //-- complete previous state from the backup file.
  h = mustOpen("resizeInt", false);
  installHooks();
  failNextOn(OP_RENAME, 1, ".rfsnew");
  failNextOn(OP_RENAME, 1, ".rfsold");
  CHECK_ERR(ringFileResize(h, 5), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);
  h = mustOpen("resizeInt", false);
  CHECK(infoCount(h) == 3);
  uint64_t k = 2;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  ringFileClose(h);

  //-- Simulate a crash between the renames: path was moved to .rfsold and
  //-- the candidate sits at .rfsnew. Reopen must pick a complete state.
  ringPath(path, sizeof(path), "resizeInt");
  snprintf(oldPath, sizeof(oldPath), "%s.rfsold", path);
  snprintf(newPath, sizeof(newPath), "%s.rfsnew", path);
  CHECK(rename(path, oldPath) == 0);
  copyFile(oldPath, newPath);
  CHECK(!pathExists(path));
  h = mustOpen("resizeInt", false);
  CHECK(infoCount(h) == 3);
  CHECK(pathExists(path));
  ringFileClose(h);

  //-- path valid while a stale candidate and backup linger: main file wins
  //-- and the writable reopen cleans up the strays.
  ringPath(path, sizeof(path), "resizeInt");
  copyFile(path, newPath);
  copyFile(path, oldPath);
  h = mustOpen("resizeInt", false);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);
  CHECK(!pathExists(newPath));
  CHECK(!pathExists(oldPath));

  //-- Read-only open on a state needing recovery reports it explicitly.
  CHECK(rename(path, oldPath) == 0);
  ringFileOpenConfig_t ocfg = meterOpen(true, RING_FILE_PERSIST_IMMEDIATE, 0);
  ringFileHandle_t *ro = NULL;
  CHECK_ERR(ringFileOpen(path, &ocfg, &ro), RING_FILE_STORE_ERR_RECOVERY_REQUIRED);
  CHECK(ro == NULL);
  ringFileClose(ro);
  CHECK(rename(oldPath, path) == 0);
}

static void testFailureInjectionMutations(void)
{
  ringFileHandle_t *h = mustCreate("failadd", 4);
  meterRec_t rec, out;
  uint64_t k = 1;

  mkMeter(&rec, 1, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

  //-- Slot write failure during add.
  installHooks();
  failNext(OP_WRITE, 1);
  mkMeter(&rec, 2, 2, 0);
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_IO);
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_IO_FAULTED);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), RING_FILE_STORE_ERR_IO_FAULTED);
  CHECK_ERR(ringFileSync(h), RING_FILE_STORE_ERR_IO_FAULTED);
  ringFileClose(h);
  h = mustOpen("failadd", false);
  CHECK(infoCount(h) == 1);
  k = 2;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  ringFileClose(h);

  //-- fsync failure after a committed slot write: the record reached the
  //-- file, so the reopened state is the complete new state.
  h = mustOpen("failadd", false);
  installHooks();
  failNext(OP_FSYNC, 1);
  k = 2;
  mkMeter(&rec, 2, 2, 0);
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);
  h = mustOpen("failadd", false);
  CHECK(infoCount(h) == 2);
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  ringFileClose(h);

  //-- Update: write failure leaves the previous payload.
  h = mustOpen("failadd", false);
  installHooks();
  failNext(OP_WRITE, 1);
  k = 1;
  mkMeter(&rec, 1, 999, 0);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);
  h = mustOpen("failadd", false);
  k = 1;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 1);
  ringFileClose(h);

  //-- Clear fsync failure: complete old state or clear, never partial.
  h = mustOpen("failadd", false);
  installHooks();
  failNext(OP_FSYNC, 1);
  CHECK_ERR(ringFileClear(h), RING_FILE_STORE_ERR_IO);
  ringFileClose(h);
  h = mustOpen("failadd", false);
  uint32_t count = infoCount(h);
  CHECK(count == 0 || count == 2);
  ringFileClose(h);
}

static void testUnchangedWrites(void)
{
  ringFileHandle_t *h = mustCreate("nowrite", 4);
  meterRec_t rec;
  uint64_t k = 1;

  mkMeter(&rec, 1, 100, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

  ringFileStoreResetIoStats();
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  CHECK_ERR(ringFileUpsert(h, &rec), ESP_OK);
  rfsIoStats_t stats = lastStats();
  CHECK(stats.writes == 0);
  CHECK(stats.fsyncs == 0);

  //-- Reads and a clean close never write.
  ringFileStoreResetIoStats();
  meterRec_t out;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK_ERR(ringFileSync(h), ESP_OK);
  CHECK_ERR(ringFileReadOldest(h, &out), ESP_OK);
  CHECK(lastStats().writes == 0);
  ringFileClose(h);

  ringFileStoreResetIoStats();
  h = mustOpen("nowrite", false);
  CHECK_ERR(ringFileSync(h), ESP_OK);
  CHECK(lastStats().fsyncs == 0);
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  ringFileClose(h);
  CHECK(lastStats().writes == 0);
}

static void testWearDistribution(void)
{
  ringFileHandle_t *h = mustCreate("wear", 4);
  meterRec_t rec;

  ringFileStoreResetIoStats();

  for (uint64_t ts = 1; ts <= 40; ts++)
  {
    mkMeter(&rec, ts, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  rfsIoStats_t stats = lastStats();
  //-- One slot write + one sync per add; header untouched.
  CHECK(stats.writes == 40);
  CHECK(stats.writeBytes == 40 * (SLOT_HDR_SIZE + (uint32_t)sizeof(meterRec_t)));
  CHECK(stats.fsyncs == 40);

  //-- Repeated edits of a single key: exactly one write + one sync each.
  //-- Key 38 is still active after the 40 wraps (cap 4 keeps 37..40).
  ringFileStoreResetIoStats();
  uint64_t k = 38;

  for (int i = 0; i < 30; i++)
  {
    mkMeter(&rec, 38, (uint32_t)i, 0);
    CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  }

  stats = lastStats();
  CHECK(stats.writes == 30);
  CHECK(stats.fsyncs == 30);
  ringFileClose(h);
}

static void testBufferedMode(void)
{
  ringFileHandle_t *h = mustCreate("buffered", 3);
  meterRec_t rec, out;

  ringFileClose(h);
  h = mustOpenMode("buffered", false, RING_FILE_PERSIST_BUFFERED, 4);

  ringFileStoreResetIoStats();
  mkMeter(&rec, 1, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 2, 2, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);

  //-- Staged adds are visible through the same handle with no storage yet.
  uint64_t k = 1;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 1);
  CHECK(infoCount(h) == 2);
  rfsIoStats_t stats = lastStats();
  CHECK(stats.writes == 0);
  CHECK(stats.fsyncs == 0);

  //-- Duplicate check works on staged data.
  CHECK_ERR(ringFileAdd(h, &rec), RING_FILE_STORE_ERR_DUPLICATE_KEY);

  //-- Repeated updates of the same staged record coalesce.
  for (int i = 0; i < 5; i++)
  {
    mkMeter(&rec, 1, (uint32_t)(10 + i), 0);
    CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  }

  ringFileInfo_t info;
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.pendingCount == 2);
  CHECK(info.dirty);
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 14);

  CHECK_ERR(ringFileSync(h), ESP_OK);
  stats = lastStats();
  CHECK(stats.writes == 2); //-- coalesced: one image per staged record
  CHECK(stats.fsyncs == 1);

  //-- Eviction semantics apply before any flush.
  mkMeter(&rec, 3, 3, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, 4, 4, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK); //-- capacity 3: evicts key 1
  CHECK(infoCount(h) == 3);
  k = 1;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  k = 4;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  ringFileClose(h);

  //-- Reopen matches the last logical state.
  h = mustOpen("buffered", false);
  CHECK(infoCount(h) == 3);
  k = 2;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 2);
  k = 3;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  k = 4;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);

  //-- Buffered with pending data: resize commits first, clear drops staged.
  k = 2;
  mkMeter(&rec, 2, 22, 0);
  CHECK_ERR(ringFileUpdateByKey(h, &k, &rec), ESP_OK);
  CHECK_ERR(ringFileResize(h, 5), ESP_OK);
  k = 2;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  CHECK(out.energyWh == 22);

  mkMeter(&rec, 99, 9, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK(infoCount(h) == 4);
  CHECK_ERR(ringFileClear(h), ESP_OK);
  CHECK(infoCount(h) == 0);
  ringFileClose(h);

  h = mustOpen("buffered", false);
  CHECK(infoCount(h) == 0);
  ringFileClose(h);
}

static void testBufferedOverflowFlushes(void)
{
  ringFileHandle_t *h = mustCreate("bover", 10);
  meterRec_t rec, out;
  ringFileClose(h);

  h = mustOpenMode("bover", false, RING_FILE_PERSIST_BUFFERED, 3);
  ringFileStoreResetIoStats();

  for (uint64_t ts = 1; ts <= 6; ts++)
  {
    mkMeter(&rec, ts, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  ringFileClose(h);
  rfsIoStats_t stats = lastStats();
  //-- Buffer limit 3 forces two flushes (6 records): one on overflow, one
  //-- on the successful close.
  CHECK(stats.writes == 6);
  CHECK(stats.fsyncs == 2);

  h = mustOpen("bover", false);
  CHECK(infoCount(h) == 6);
  uint64_t k = 6;
  CHECK_ERR(ringFileReadByKey(h, &k, &out), ESP_OK);
  ringFileClose(h);
}

static void testMultipleFiles(void)
{
  ringFileHandle_t *a = mustCreate("multiA", 2);
  ringFileHandle_t *b = mustCreate("multiB", 3);
  meterRec_t rec, out;

  mkMeter(&rec, 1, 11, 0);
  CHECK_ERR(ringFileAdd(a, &rec), ESP_OK);
  mkMeter(&rec, 2, 22, 0);
  CHECK_ERR(ringFileAdd(b, &rec), ESP_OK);
  CHECK(infoCount(a) == 1);
  CHECK(infoCount(b) == 1);
  uint64_t k = 2;
  CHECK_ERR(ringFileReadByKey(a, &k, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  CHECK_ERR(ringFileReadByKey(b, &k, &out), ESP_OK);
  ringFileClose(a);
  ringFileClose(b);
}

static void testValidateGoodRing(void)
{
  ringFileHandle_t *h = mustCreate("valid", 4);
  meterRec_t rec;

  for (uint64_t ts = 1; ts <= 3; ts++)
  {
    mkMeter(&rec, ts, (uint32_t)ts, 0);
    CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  }

  ringFileClose(h);

  char path[300];
  ringPath(path, sizeof(path), "valid");
  ringFileValidation_t v;
  CHECK_ERR(ringFileValidate(path, &v), ESP_OK);
  CHECK(v.headerValid);
  CHECK(v.metadataIssue == RING_FILE_META_OK);
  CHECK(v.capacity == 4);
  CHECK(v.recordSize == sizeof(meterRec_t));
  CHECK(v.slotCount == 5);
  CHECK(v.badSlotCount == 0);
  CHECK(v.committedCount == 3);
  CHECK(!v.strayResizeFiles);
}

//-- Ring with more records (capacity 10): fills the ring, wraps twice,
//-- updates a middle record and verifies insertion order. Every command
//-- (with its data) and its result, plus the key/slot dumps, go to
//-- testLog.md.
static void testMoreRecords(void)
{
  ringFileHandle_t *h = mustCreate("more10", 10);
  meterRec_t rec, out;

  for (uint64_t i = 1; i <= 12; i++)
  {
    uint64_t ts = i * 100;
    mkMeter(&rec, ts, (uint32_t)(i * 10), (uint16_t)i);
    esp_err_t err = ringFileAdd(h, &rec);
    char step[64];
    snprintf(step, sizeof(step), "add key=%llu", (unsigned long long)ts);
    logCmd(err, "ringFileAdd(key=%llu, energyWh=%lu, errorCount=%u) "
           "on a capacity-10 ring", (unsigned long long)ts,
           (unsigned long)(i * 10), (unsigned)i);
    CHECK_ERR(err, ESP_OK);
    logRingDump(h, step);
  }

  CHECK(infoCount(h) == 10);

  //-- The 11th and 12th add evicted exactly the two oldest (100 and 200).
  uint64_t k = 100;
  esp_err_t err = ringFileReadByKey(h, &k, &out);
  CHECK_ERR(err, RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  logCmd(err, "ringFileReadByKey(key=100) — evicted by the 11th add");
  k = 200;
  err = ringFileReadByKey(h, &k, &out);
  CHECK_ERR(err, RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  logCmd(err, "ringFileReadByKey(key=200) — evicted by the 12th add");

  for (uint64_t i = 3; i <= 12; i++)
  {
    k = i * 100;
    err = ringFileReadByKey(h, &k, &out);
    CHECK_ERR(err, ESP_OK);
    CHECK(out.energyWh == (uint32_t)(i * 10));
  }

  logCmdText("keys 300..1200 active, energyWh correct",
             "ringFileReadByKey for all 10 active keys (300..1200)");

  err = ringFileReadOldest(h, &out);
  CHECK_ERR(err, ESP_OK);
  CHECK(out.timestamp == 300);
  logCmd(err, "ringFileReadOldest()");

  err = ringFileReadNewest(h, &out);
  CHECK_ERR(err, ESP_OK);
  CHECK(out.timestamp == 1200);
  logCmd(err, "ringFileReadNewest()");

  //-- Update a middle record: position must be preserved.
  k = 500;
  mkMeter(&rec, 500, 999, 7);
  err = ringFileUpdateByKey(h, &k, &rec);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileUpdateByKey(key=500, energyWh=999, errorCount=7) "
         "— position preserved");
  logRingDump(h, "update key=500");

  k = 500;
  err = ringFileReadByKey(h, &k, &out);
  CHECK_ERR(err, ESP_OK);
  CHECK(out.energyWh == 999 && out.errorCount == 7);

  ringFileClose(h);

  //-- Reopen: same 10 keys, same insertion order.
  h = mustOpen("more10", false);
  CHECK(infoCount(h) == 10);
  logRingDump(h, "reopen after close");
  ringFileClose(h);
}

//-- Significant key positions: the keyPositionMask marks which byte
//-- positions of the key take part in every comparison.
static void testKeyPositionMask(void)
{
  //-- Bit i set marks byte position i as significant: for the 8-byte
  //-- timestamp key only bytes 2..7 take part; bytes 0 and 1 are ignored.
  static const uint64_t maskValue = 0x00000000000000FCULL;
  char path[300];
  ringPath(path, sizeof(path), "keymask");
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 4;
  cfg.schemaId = 3;
  cfg.schemaVersion = 1;
  cfg.recordSize = sizeof(meterRec_t);
  cfg.key.keyOffset = (uint32_t)offsetof(meterRec_t, timestamp);
  cfg.key.keyType = RING_FILE_KEY_U64;
  cfg.key.keySize = (uint16_t)sizeof(uint64_t);
  cfg.key.keyPositionMask = maskValue;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;

  ringFileHandle_t *h = NULL;
  esp_err_t err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileCreate(capacity=4, key=timestamp u64, "
         "keyPositionMask=0x%016llx: byte positions 0-1 not significant)",
         (unsigned long long)maskValue);

  ringFileInfo_t info;
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.keyPositionMask == maskValue);

  meterRec_t rec, out;
  uint64_t base = 0x12340001ULL;
  mkMeter(&rec, base, 10, 0);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileAdd(key=0x%016llx)", (unsigned long long)base);

  //-- Differs only at insignificant positions → same key → duplicate.
  uint64_t same = 0x123400ABULL;
  mkMeter(&rec, same, 20, 0);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, RING_FILE_STORE_ERR_DUPLICATE_KEY);
  logCmd(err, "ringFileAdd(key=0x%016llx: only bytes 0-1 differ)",
         (unsigned long long)same);

  uint64_t look = 0x123400FFULL;
  err = ringFileReadByKey(h, &look, &out);
  CHECK_ERR(err, ESP_OK);
  CHECK(out.energyWh == 10);
  logCmd(err, "ringFileReadByKey(key=0x%016llx) finds key=0x%016llx "
         "(byte 0 ignored)", (unsigned long long)look,
         (unsigned long long)base);

  uint64_t other = 0x12350000ULL;
  err = ringFileReadByKey(h, &other, &out);
  CHECK_ERR(err, RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  logCmd(err, "ringFileReadByKey(key=0x%016llx): byte 2 (significant) "
         "differs", (unsigned long long)other);

  //-- Embedded key differing from the lookup key only in insignificant
  //-- positions is accepted.
  uint64_t lk = 0x12340002ULL;
  mkMeter(&rec, 0x12340003ULL, 33, 1);
  err = ringFileUpdateByKey(h, &lk, &rec);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileUpdateByKey(key=0x%016llx, embedded key "
         "0x%016llx: only byte 0 differs): accepted",
         (unsigned long long)lk, (unsigned long long)0x12340003ULL);

  uint64_t back = 0x12340001ULL;
  err = ringFileReadByKey(h, &back, &out);
  CHECK_ERR(err, ESP_OK);
  CHECK(out.energyWh == 33 && out.errorCount == 1);
  logCmd(err, "ringFileReadByKey(key=0x%016llx) shows the update",
         (unsigned long long)back);

  logRingDump(h, "keymask scenario (adds, duplicate, update)");
  ringFileClose(h);

  //-- Reopen: the mask is restored from the header and keeps working.
  ringFileOpenConfig_t ocfg;
  memset(&ocfg, 0, sizeof(ocfg));
  ocfg.expectedSchemaId = 3;
  ocfg.expectedSchemaVersion = 1;
  ocfg.expectedRecordSize = sizeof(meterRec_t);
  err = ringFileOpen(path, &ocfg, &h);
  CHECK_ERR(err, ESP_OK);
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.keyPositionMask == maskValue);
  err = ringFileReadByKey(h, &look, &out);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "reopen: keyPositionMask from header, "
         "readByKey(0x%016llx) finds the record", (unsigned long long)look);
  ringFileClose(h);

  //-- Invalid masks are rejected at create time.
  ringPath(path, sizeof(path), "keymaskbad1");
  cfg = meterConfig(3);
  cfg.key.keyPositionMask = 0x1FFULL; //-- bit 8 set while keySize is 8
  err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_ERR_INVALID_ARG);
  logCmd(err, "ringFileCreate keyPositionMask=0x1FF (bit beyond "
         "keySize=8) -> rejected");

  ringPath(path, sizeof(path), "keymaskbad2");
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 3;
  cfg.schemaId = 3;
  cfg.schemaVersion = 1;
  cfg.recordSize = sizeof(bigRec_t);
  cfg.key.keyOffset = 0;
  cfg.key.keyType = RING_FILE_KEY_BYTES;
  cfg.key.keySize = 80;
  cfg.key.keyPositionMask = 1;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_ERR_INVALID_ARG);
  logCmd(err, "ringFileCreate BYTES keySize=80 with keyPositionMask "
         "(mask limited to positions 0-63) -> rejected");
}

//-- ---------------------------------------------------------------------------
//-- Timestamp helpers and TIMEBUCKET addressing
//-- ---------------------------------------------------------------------------

static uint32_t testCrc32(const uint8_t *data, size_t len)
{
  uint32_t state = 0xFFFFFFFFu;

  for (size_t i = 0; i < len; i++)
  {
    state ^= data[i];

    for (int bit = 0; bit < 8; bit++)
    {
      state = (state & 1u) ? ((state >> 1) ^ 0xEDB88320u) : (state >> 1);
    }
  }

  return ~state;
}

static void testTimeHelpers(void)
{
  uint64_t ts = 0;

  //-- Known anchor values (unix seconds, wall clock treated as UTC).
  CHECK_ERR(ringFileTimeStampStringToUnix("20000101000000", &ts), ESP_OK);
  CHECK(ts == 946684800ULL);
  logCmd(ESP_OK, "ringFileTimeStampStringToUnix(\"20000101000000\") = 946684800");

  CHECK_ERR(ringFileTimeStampStringToUnix("20240229000000", &ts), ESP_OK);
  CHECK(ts == 1709164800ULL);
  logCmd(ESP_OK, "ringFileTimeStampStringToUnix(\"20240229000000\") leap day "
         "= 1709164800");

  CHECK_ERR(ringFileTimeStampStringToUnix("20991231235959", &ts), ESP_OK);
  CHECK(ts == 4102444799ULL);

  CHECK_ERR(ringFileTimeStampStringToUnix("20261023123300", &ts), ESP_OK);

  //-- Rejections: invalid second/minute/hour/month/day, wrong century,
  //-- non-digits, wrong length, NULL.
  CHECK_ERR(ringFileTimeStampStringToUnix("20261023123378", &ts), ESP_ERR_INVALID_ARG);
  logCmd(ESP_ERR_INVALID_ARG, "\"20261023123378\": second 78 rejected");
  CHECK_ERR(ringFileTimeStampStringToUnix("20261023126000", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("20261023240000", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("20261301000000", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("20261032000000", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("20230229000000", &ts), ESP_ERR_INVALID_ARG);
  logCmd(ESP_ERR_INVALID_ARG, "\"20230229000000\": 2023 is not a leap year");
  CHECK_ERR(ringFileTimeStampStringToUnix("21000101000000", &ts), ESP_ERR_INVALID_ARG);
  logCmd(ESP_ERR_INVALID_ARG, "\"21000101000000\": century 21 rejected");
  CHECK_ERR(ringFileTimeStampStringToUnix("19991231235959", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("2026102312330a", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("2026102312330", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("202610231233000", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix("", &ts), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileTimeStampStringToUnix(NULL, &ts), ESP_ERR_INVALID_ARG);

  //-- Inverse conversion and round-trip.
  char stamp[15];
  memset(stamp, 0, sizeof(stamp));
  CHECK_ERR(ringFileUnixToTimeStampString(946684800ULL, stamp), ESP_OK);
  CHECK(strcmp(stamp, "20000101000000") == 0);
  CHECK_ERR(ringFileUnixToTimeStampString(1709164800ULL, stamp), ESP_OK);
  CHECK(strcmp(stamp, "20240229000000") == 0);
  CHECK_ERR(ringFileUnixToTimeStampString(4102444799ULL, stamp), ESP_OK);
  CHECK(strcmp(stamp, "20991231235959") == 0);
  CHECK_ERR(ringFileUnixToTimeStampString(946684799ULL, stamp), ESP_ERR_INVALID_ARG);
  logCmd(ESP_ERR_INVALID_ARG, "ringFileUnixToTimeStampString(946684799): "
         "before 2000 rejected");
  CHECK_ERR(ringFileUnixToTimeStampString(4102444800ULL, stamp), ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUnixToTimeStampString(946684800ULL, NULL), ESP_ERR_INVALID_ARG);

  CHECK_ERR(ringFileUnixToTimeStampString(ts, stamp), ESP_OK);
  CHECK(strcmp(stamp, "20261023123300") == 0);
  logCmd(ESP_OK, "round-trip unix -> \"20261023123300\"");

  //-- Buckets: hour and day are pure divisions; the week bucket starts
  //-- Monday 00:00 and the month bucket uses (EEYY-1)*12+MM.
  uint64_t sunday = 0, monday = 0;
  CHECK(ringFileTimeStampStringToUnix("20260104123300", &sunday) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20260105000000", &monday) == ESP_OK);

  uint64_t bHour = 0, bDay = 0, bWeekSun = 0, bWeekMon = 0, bMonth = 0;
  CHECK_ERR(ringFileUnixToBucketId(sunday, RING_FILE_GRANULARITY_HOUR, &bHour),
            ESP_OK);
  CHECK(bHour == sunday / 3600ULL);
  CHECK_ERR(ringFileUnixToBucketId(sunday, RING_FILE_GRANULARITY_DAY, &bDay), ESP_OK);
  CHECK(bDay == sunday / 86400ULL);
  CHECK_ERR(ringFileUnixToBucketId(sunday, RING_FILE_GRANULARITY_WEEK, &bWeekSun),
            ESP_OK);
  CHECK_ERR(ringFileUnixToBucketId(monday, RING_FILE_GRANULARITY_WEEK, &bWeekMon),
            ESP_OK);
  CHECK((monday + 259200ULL) % 604800ULL == 0ULL);
  CHECK(bWeekSun + 1ULL == bWeekMon);
  logCmd(ESP_OK, "week buckets: Sunday 2026-01-04 -> %llu, Monday "
         "2026-01-05 -> %llu (Monday starts the bucket)",
         (unsigned long long)bWeekSun, (unsigned long long)bWeekMon);

  CHECK_ERR(ringFileUnixToBucketId(ts, RING_FILE_GRANULARITY_MONTH, &bMonth), ESP_OK);
  CHECK(bMonth == (2026u - 1u) * 12u + 10u);
  logCmd(ESP_OK, "month bucket 2026-10 = (2026-1)*12+10 = %llu",
         (unsigned long long)bMonth);
  CHECK_ERR(ringFileUnixToBucketId(ts, RING_FILE_GRANULARITY_NONE, &bMonth),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUnixToBucketId(ts, (ringFileGranularity_t)9, &bMonth),
            ESP_ERR_INVALID_ARG);
  CHECK_ERR(ringFileUnixToBucketId(ts, RING_FILE_GRANULARITY_HOUR, NULL),
            ESP_ERR_INVALID_ARG);

  //-- ISO-8601 weeks: weeks start Monday; 2025-12-29 already belongs to
  //-- ISO 2026-W01 (that week's Thursday is 2026-01-01).
  uint64_t xmas29 = 0;
  CHECK(ringFileTimeStampStringToUnix("20251229000000", &xmas29) == ESP_OK);
  uint32_t isoYear = 0, isoWeek = 0;
  CHECK_ERR(ringFileUnixToIsoWeek(xmas29, &isoYear, &isoWeek), ESP_OK);
  CHECK(isoYear == 2026 && isoWeek == 1);
  logCmd(ESP_OK, "ringFileUnixToIsoWeek(2025-12-29) = %u-W%02u",
         (unsigned)isoYear, (unsigned)isoWeek);
  CHECK_ERR(ringFileUnixToIsoWeek(sunday, &isoYear, &isoWeek), ESP_OK);
  CHECK(isoYear == 2026 && isoWeek == 1);
  CHECK_ERR(ringFileUnixToIsoWeek(monday, &isoYear, &isoWeek), ESP_OK);
  CHECK(isoYear == 2026 && isoWeek == 2);
  CHECK_ERR(ringFileUnixToIsoWeek(xmas29, NULL, &isoWeek), ESP_ERR_INVALID_ARG);
}

static ringFileConfig_t timeBucketConfig(uint32_t capacity,
                                         ringFileGranularity_t granularity,
                                         ringFilePersistMode_t persistMode,
                                         uint16_t maxPending)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = capacity;
  cfg.schemaId = 77;
  cfg.schemaVersion = 1;
  cfg.recordSize = sizeof(meterRec_t);
  cfg.persistMode = persistMode;
  cfg.maxPendingRecords = maxPending;
  cfg.addressing = RING_FILE_ADDRESSING_TIMEBUCKET;
  cfg.granularity = granularity;
  cfg.timestampOffset = (uint32_t)offsetof(meterRec_t, timestamp);
  return cfg;
}

static ringFileOpenConfig_t timeBucketOpen(ringFileGranularity_t granularity)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = 77;
  cfg.expectedSchemaVersion = 1;
  cfg.expectedRecordSize = sizeof(meterRec_t);
  cfg.expectedAddressing = RING_FILE_ADDRESSING_TIMEBUCKET;
  cfg.expectedGranularity = granularity;
  cfg.expectedTimestampOffset = (uint32_t)offsetof(meterRec_t, timestamp);
  return cfg;
}

static void testTimeBucketRing(void)
{
  char path[300];
  ringPath(path, sizeof(path), "timebucket");

  ringFileConfig_t cfg = timeBucketConfig(3, RING_FILE_GRANULARITY_HOUR,
                                          RING_FILE_PERSIST_IMMEDIATE, 0);
  ringFileHandle_t *h = NULL;
  esp_err_t err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileCreate(TIMEBUCKET, HOUR, capacity=3, "
         "timestampOffset=%u)", (unsigned)cfg.timestampOffset);

  ringFileInfo_t info;
  CHECK(ringFileGetInfo(h, &info) == ESP_OK);
  CHECK(info.addressing == RING_FILE_ADDRESSING_TIMEBUCKET);
  CHECK(info.granularity == RING_FILE_GRANULARITY_HOUR);
  CHECK(info.timestampOffset == (uint32_t)offsetof(meterRec_t, timestamp));

  meterRec_t rec, out;
  uint64_t tsH09 = 0, tsH10 = 0, tsH10late = 0, tsH11 = 0, tsH12 = 0, tsH13 = 0;
  CHECK(ringFileTimeStampStringToUnix("20261023090000", &tsH09) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261023100000", &tsH10) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261023105959", &tsH10late) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261023110000", &tsH11) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261023120000", &tsH12) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261023130000", &tsH13) == ESP_OK);

  uint64_t bH09 = 0, bH10 = 0, bH11 = 0, bH12 = 0, bH13 = 0;
  CHECK(ringFileUnixToBucketId(tsH09, RING_FILE_GRANULARITY_HOUR, &bH09) == ESP_OK);
  CHECK(ringFileUnixToBucketId(tsH10, RING_FILE_GRANULARITY_HOUR, &bH10) == ESP_OK);
  CHECK(ringFileUnixToBucketId(tsH11, RING_FILE_GRANULARITY_HOUR, &bH11) == ESP_OK);
  CHECK(ringFileUnixToBucketId(tsH12, RING_FILE_GRANULARITY_HOUR, &bH12) == ESP_OK);
  CHECK(ringFileUnixToBucketId(tsH13, RING_FILE_GRANULARITY_HOUR, &bH13) == ESP_OK);
  CHECK(bH09 + 1 == bH10 && bH10 + 1 == bH11 && bH11 + 1 == bH12 &&
        bH12 + 1 == bH13);

  mkMeter(&rec, tsH10, 100, 0);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileAdd(hour 10)");

  mkMeter(&rec, tsH11, 110, 0);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, ESP_OK);
  logCmd(err, "ringFileAdd(hour 11)");

  //-- Same bucket: many timestamps map to hour 10; add overwrites.
  mkMeter(&rec, tsH10late, 105, 1);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, ESP_OK);
  CHECK(infoCount(h) == 2);
  CHECK_ERR(ringFileReadByKey(h, &bH10, &out), ESP_OK);
  CHECK(out.energyWh == 105 && out.errorCount == 1);
  logCmd(err, "ringFileAdd(hour 10 again at 10:59:59): same bucket "
         "overwritten");

  //-- Inactive older bucket: clock rewind through add is refused.
  mkMeter(&rec, tsH09, 90, 0);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, RING_FILE_STORE_ERR_NOT_NEWER);
  CHECK_ERR(ringFileReadByKey(h, &bH09, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  logCmd(err, "ringFileAdd(hour 09: not newer -> refused, nothing stored");

  //-- Upsert updates an active bucket for any timestamp.
  mkMeter(&rec, tsH10, 106, 0);
  err = ringFileUpsert(h, &rec);
  CHECK_ERR(err, ESP_OK);
  CHECK_ERR(ringFileReadByKey(h, &bH10, &out), ESP_OK);
  CHECK(out.energyWh == 106);
  logCmd(err, "ringFileUpsert(hour 10 at 10:00:00): active bucket updated "
         "with any timestamp");

  //-- Upsert add path follows the newer rule.
  mkMeter(&rec, tsH09, 90, 0);
  err = ringFileUpsert(h, &rec);
  CHECK_ERR(err, RING_FILE_STORE_ERR_NOT_NEWER);
  logCmd(err, "ringFileUpsert(hour 09 as new bucket): not newer -> refused");

  //-- Order follows bucketId, not insertion seq.
  meterRec_t oldest, newest;
  CHECK_ERR(ringFileReadOldest(h, &oldest), ESP_OK);
  CHECK(oldest.timestamp == tsH10);
  CHECK_ERR(ringFileReadNewest(h, &newest), ESP_OK);
  CHECK(newest.timestamp == tsH11);

  //-- Edit: append a newer bucket.
  mkMeter(&rec, tsH12, 120, 0);
  err = ringFileEdit(h, &rec);
  CHECK_ERR(err, ESP_OK);
  CHECK(infoCount(h) == 3);
  logCmd(err, "ringFileEdit(hour 12): newer bucket appended");

  //-- Edit for an evicted-ago bucket: outside the retained window
  //-- [newestBucket - capacity + 1, newestBucket] = [bucket(10), bucket(12)].
  mkMeter(&rec, tsH09, 90, 0);
  err = ringFileEdit(h, &rec);
  CHECK_ERR(err, RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE);
  CHECK(infoCount(h) == 3);
  logCmd(err, "ringFileEdit(hour 09): outside retained window -> rejected");

  //-- Edit an active bucket with any timestamp.
  mkMeter(&rec, tsH10, 107, 2);
  err = ringFileEdit(h, &rec);
  CHECK_ERR(err, ESP_OK);
  CHECK_ERR(ringFileReadByKey(h, &bH10, &out), ESP_OK);
  CHECK(out.energyWh == 107 && out.errorCount == 2);
  logCmd(err, "ringFileEdit(hour 10): active bucket updated");

  //-- Eviction drops the oldest bucket.
  mkMeter(&rec, tsH13, 130, 0);
  err = ringFileAdd(h, &rec);
  CHECK_ERR(err, ESP_OK);
  CHECK(infoCount(h) == 3);
  CHECK_ERR(ringFileReadOldest(h, &oldest), ESP_OK);
  CHECK(oldest.timestamp == tsH11);
  CHECK_ERR(ringFileReadNewest(h, &newest), ESP_OK);
  CHECK(newest.timestamp == tsH13);
  CHECK_ERR(ringFileReadByKey(h, &bH10, &out), RING_FILE_STORE_ERR_KEY_NOT_FOUND);
  logCmd(err, "ringFileAdd(hour 13): oldest bucket (hour 10) evicted");

  //-- Iterator walks by bucket order; GetSlot reports bucketId % capacity.
  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  meterRec_t step;
  uint64_t expectTs[3] = {tsH11, tsH12, tsH13};
  uint64_t expectBucket[3] = {bH11, bH12, bH13};

  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &step),
            ESP_OK);

  for (int i = 0; i < 3; i++)
  {
    CHECK(step.timestamp == expectTs[i]);
    uint32_t slot = RING_FILE_SLOT_STAGED;
    CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
    CHECK(slot == (uint32_t)(expectBucket[i] % 3u));

    if (i < 2)
    {
      CHECK_ERR(ringFileIteratorNext(h, &it, &step), ESP_OK);
    }
  }

  CHECK_ERR(ringFileIteratorNext(h, &it, &step), RING_FILE_STORE_ERR_ITER_END);
  logCmd(ESP_OK, "iterator: bucket order, logical slot = bucketId %% capacity");

  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_KEY, &bH12, &step), ESP_OK);
  CHECK(step.timestamp == tsH12);
  logCmd(ESP_OK, "ringFileIteratorBegin(KEY, bucketId of hour 12)");

  logRingDump(h, "TIMEBUCKET scenario (adds, overwrite, edit, eviction)");
  ringFileClose(h);

  //-- Reopen with matching expectations; every mismatch is a schema error.
  ringFileOpenConfig_t ocfg = timeBucketOpen(RING_FILE_GRANULARITY_HOUR);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), ESP_OK);
  CHECK(infoCount(h) == 3);
  ringFileClose(h);

  ocfg = timeBucketOpen(RING_FILE_GRANULARITY_DAY);
  err = ringFileOpen(path, &ocfg, &h);
  CHECK_ERR(err, RING_FILE_STORE_ERR_SCHEMA_MISMATCH);
  logCmd(err, "reopen expecting DAY granularity -> schema mismatch");

  ocfg = timeBucketOpen(RING_FILE_GRANULARITY_HOUR);
  ocfg.expectedAddressing = RING_FILE_ADDRESSING_INSERTION;
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  ocfg = timeBucketOpen(RING_FILE_GRANULARITY_HOUR);
  ocfg.expectedTimestampOffset = 4;
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), RING_FILE_STORE_ERR_SCHEMA_MISMATCH);

  //-- Resize keeps the newest buckets.
  ocfg = timeBucketOpen(RING_FILE_GRANULARITY_HOUR);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), ESP_OK);
  CHECK_ERR(ringFileResize(h, 2), ESP_OK);
  CHECK(infoCount(h) == 2);
  CHECK_ERR(ringFileReadOldest(h, &oldest), ESP_OK);
  CHECK(oldest.timestamp == tsH12);
  CHECK_ERR(ringFileReadNewest(h, &newest), ESP_OK);
  CHECK(newest.timestamp == tsH13);
  logCmd(ESP_OK, "ringFileResize(2): newest buckets (12, 13) retained");
  ringFileClose(h);

  //-- ringFileEdit on an insertion-mode ring is rejected.
  ringPath(path, sizeof(path), "timeins");
  ringFileConfig_t insCfg = meterConfig(3);
  CHECK_ERR(ringFileCreate(path, &insCfg, &h), ESP_OK);
  mkMeter(&rec, 5, 5, 0);
  err = ringFileEdit(h, &rec);
  CHECK_ERR(err, ESP_ERR_INVALID_STATE);
  logCmd(err, "ringFileEdit on INSERTION ring -> INVALID_STATE");
  ringFileClose(h);

  //-- Invalid TIMEBUCKET configurations are rejected at create.
  cfg = timeBucketConfig(3, RING_FILE_GRANULARITY_HOUR,
                         RING_FILE_PERSIST_IMMEDIATE, 0);
  cfg.timestampOffset = (uint32_t)sizeof(meterRec_t) - 4u;  //-- 8 bytes do not fit
  err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_ERR_INVALID_ARG);
  logCmd(err, "create TIMEBUCKET with timestampOffset+8 > recordSize -> rejected");

  cfg = timeBucketConfig(3, RING_FILE_GRANULARITY_NONE,
                         RING_FILE_PERSIST_IMMEDIATE, 0);
  err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_ERR_INVALID_ARG);

  cfg = timeBucketConfig(3, (ringFileGranularity_t)9, RING_FILE_PERSIST_IMMEDIATE, 0);
  err = ringFileCreate(path, &cfg, &h);
  CHECK_ERR(err, ESP_ERR_INVALID_ARG);
  logCmd(err, "create TIMEBUCKET with invalid granularity -> rejected");
}

static void testTimeBucketBufferedAndV2(void)
{
  //-- Buffered TIMEBUCKET: staged reads see the overlay, the staged slot
  //-- marker is reported, a flush commits and reopen shows the day records.
  char path[300];
  ringPath(path, sizeof(path), "timebuf");
  ringFileConfig_t cfg = timeBucketConfig(4, RING_FILE_GRANULARITY_DAY,
                                          RING_FILE_PERSIST_BUFFERED, 4);
  ringFileHandle_t *h = NULL;
  CHECK_ERR(ringFileCreate(path, &cfg, &h), ESP_OK);
  logCmd(ESP_OK, "ringFileCreate(TIMEBUCKET, DAY, buffered maxPending=4)");

  meterRec_t rec, out;
  uint64_t day1early = 0, day1late = 0, day2 = 0;
  CHECK(ringFileTimeStampStringToUnix("20261001010000", &day1early) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261001180000", &day1late) == ESP_OK);
  CHECK(ringFileTimeStampStringToUnix("20261002120000", &day2) == ESP_OK);

  mkMeter(&rec, day1early, 1, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  mkMeter(&rec, day1late, 2, 0);
  CHECK_ERR(ringFileUpsert(h, &rec), ESP_OK);   //-- same bucket → staged overwrite
  mkMeter(&rec, day2, 20, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  CHECK(infoCount(h) == 2);

  ringFileIterator_t it;
  memset(&it, 0, sizeof(it));
  meterRec_t step;
  CHECK_ERR(ringFileIteratorBegin(h, &it, RING_FILE_ITER_OLDEST, NULL, &step),
            ESP_OK);
  CHECK(step.timestamp == day1late && step.energyWh == 2);
  uint32_t slot = 0;
  CHECK_ERR(ringFileIteratorGetSlot(h, &it, &slot), ESP_OK);
  CHECK(slot == RING_FILE_SLOT_STAGED);
  logCmd(ESP_OK, "buffered: staged same-day overwrite visible, slot staged");
  CHECK_ERR(ringFileIteratorNext(h, &it, &step), ESP_OK);
  CHECK(step.timestamp == day2);
  CHECK_ERR(ringFileIteratorNext(h, &it, &step), RING_FILE_STORE_ERR_ITER_END);

  CHECK_ERR(ringFileSync(h), ESP_OK);

  //-- A fresh cursor after the flush reports the logical day bucket slot.
  uint64_t bDay2 = 0;
  CHECK(ringFileUnixToBucketId(day2, RING_FILE_GRANULARITY_DAY, &bDay2) == ESP_OK);
  ringFileIterator_t it2;
  memset(&it2, 0, sizeof(it2));
  CHECK_ERR(ringFileIteratorBegin(h, &it2, RING_FILE_ITER_KEY, &bDay2, &step),
            ESP_OK);
  CHECK_ERR(ringFileIteratorGetSlot(h, &it2, &slot), ESP_OK);
  CHECK(slot == (uint32_t)(bDay2 % 4u));
  logCmd(ESP_OK, "after sync: logical slot = bucketId %% capacity = %lu",
         (unsigned long)slot);
  ringFileClose(h);

  ringFileOpenConfig_t ocfg = timeBucketOpen(RING_FILE_GRANULARITY_DAY);
  CHECK_ERR(ringFileOpen(path, &ocfg, &h), ESP_OK);
  CHECK(infoCount(h) == 2);
  meterRec_t oldest;
  CHECK_ERR(ringFileReadOldest(h, &oldest), ESP_OK);
  CHECK(oldest.timestamp == day1late && oldest.energyWh == 2);
  ringFileClose(h);
  logCmd(ESP_OK, "reopen buffered DAY ring: two committed day records");

  //-- Format-version-2 compatibility: a v2 header (insertion semantics,
  //-- no addressing fields) still opens and behaves like before.
  ringPath(path, sizeof(path), "timev2");
  ringFileConfig_t insCfg = meterConfig(3);
  CHECK_ERR(ringFileCreate(path, &insCfg, &h), ESP_OK);
  mkMeter(&rec, 100, 1000, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  ringFileClose(h);

  int fd = open(path, O_RDWR);

  if (fd < 0)
  {
    printf("  FATAL open %s for v2 patch\n", path);
    exit(2);
  }

  uint8_t hdr[96];

  if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
  {
    printf("  FATAL read header %s\n", path);
    exit(2);
  }

  CHECK(hdr[4] == 3u);   //-- current format version
  hdr[4] = 2u;           //-- formatVersion = 2 (little-endian u16)
  hdr[5] = 0u;
  uint32_t crc = testCrc32(hdr, 88);
  hdr[88] = (uint8_t)crc;
  hdr[89] = (uint8_t)(crc >> 8);
  hdr[90] = (uint8_t)(crc >> 16);
  hdr[91] = (uint8_t)(crc >> 24);

  if (lseek(fd, 0, SEEK_SET) != 0 ||
      write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
  {
    printf("  FATAL patch header %s\n", path);
    exit(2);
  }

  close(fd);
  logCmdText("patched", "header patched: formatVersion 3 -> 2, CRC recomputed");

  ringFileOpenConfig_t v2cfg = meterOpen(false, RING_FILE_PERSIST_IMMEDIATE, 0);
  CHECK_ERR(ringFileOpen(path, &v2cfg, &h), ESP_OK);
  uint64_t k100 = 100;
  CHECK_ERR(ringFileReadByKey(h, &k100, &out), ESP_OK);
  CHECK(out.energyWh == 1000);

  //-- Insertion semantics: an out-of-order (smaller) key is accepted and
  //-- oldest/newest follow seq, not key order.
  mkMeter(&rec, 50, 500, 0);
  CHECK_ERR(ringFileAdd(h, &rec), ESP_OK);
  meterRec_t oldestRec, newestRec;
  CHECK_ERR(ringFileReadOldest(h, &oldestRec), ESP_OK);
  CHECK(oldestRec.timestamp == 100);
  CHECK_ERR(ringFileReadNewest(h, &newestRec), ESP_OK);
  CHECK(newestRec.timestamp == 50);
  ringFileClose(h);
  logCmd(ESP_OK, "v2 file: insertion semantics kept (out-of-order key "
         "accepted, oldest/newest by seq)");
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
    {"duplicatesAndWraparound", testDuplicatesAndWraparound},
    {"updateSemantics", testUpdateSemantics},
    {"keyTypes", testKeyTypes},
    {"iterator", testIterator},
    {"schemaMismatch", testSchemaMismatch},
    {"readOnly", testReadOnly},
    {"staleLockBreak", testStaleLockBreak},
    {"corruption", testCorruption},
    {"clear", testClear},
    {"resize", testResize},
    {"resizeEmptyAndErrors", testResizeEmptyAndErrors},
    {"resizeInterruptedStates", testResizeInterruptedStates},
    {"failureInjectionMutations", testFailureInjectionMutations},
    {"unchangedWrites", testUnchangedWrites},
    {"wearDistribution", testWearDistribution},
    {"bufferedMode", testBufferedMode},
    {"bufferedOverflowFlushes", testBufferedOverflowFlushes},
    {"multipleFiles", testMultipleFiles},
    {"validateGoodRing", testValidateGoodRing},
    {"moreRecords", testMoreRecords},
    {"keyPositionMask", testKeyPositionMask},
    {"timeHelpers", testTimeHelpers},
    {"timeBucketRing", testTimeBucketRing},
    {"timeBucketBufferedAndV2", testTimeBucketBufferedAndV2},
  };

  //-- testLog.md: path from argv[1] so run_tests.sh can place it in the
  //-- repository root. The log records every logged Opdracht (with data),
  //-- its Resultaat, the key/slot dumps and a per-test overview table.
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
    logLine("Per line: **Command** (with data) and its **Result**; "
            "ring dumps show the key and the physical slot number per "
            "record.");
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
