//--
//-- FileVersion: [9] 18-11-2026 - 19:40
//--
//-- ringFileStore - Example application for the ring_file_store component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//---------------------------------------------------------------------
//--
//-- Runs the same ring operations against either LittleFS (internal flash
//-- partition "storage") or a FATFS-formatted SD card, selected in
//-- menuconfig under "Ring file store example". The application owns
//-- mounting and unmounting; the component only sees VFS paths.
//--
//-- Demonstrated: create, reopen, wraparound, lookup, edit (update-or-add
//-- of the current hourly record), iteration in both directions, resize,
//-- validation and information reporting, plus a buffered-mode variant of
//-- the hourly logger showing the coalesced commit. Every dump line shows
//-- the record key and its physical slot number. A second ring demonstrates
//-- a key position mask: only the significant byte positions of the
//-- timestamp key take part in comparisons.

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ring_file_store.h"

#if CONFIG_EXAMPLE_BACKEND_LITTLEFS
#include "esp_littlefs.h"
#endif

#if CONFIG_EXAMPLE_BACKEND_SD
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *sSdCard = NULL;
#endif

static const char *TAG = "ringFileStore";

//-- Application record: a meter sample keyed by its hourly timestamp.
typedef struct
{
  uint64_t timestamp;      //-- key: hour epoch seconds
  uint32_t energyWh;       //-- cumulative energy at hour end
  uint16_t errorCount;     //-- errors during the hour
  uint16_t pad;            //-- explicit padding, always initialized
} meterRecord_t;

#define SCHEMA_ID 0x4D455431u  //-- "MET1"
#define SCHEMA_VERSION 1u
#define RING_CAPACITY 10u

static esp_err_t mountStorage(void)
{
#if CONFIG_EXAMPLE_BACKEND_LITTLEFS
  esp_vfs_littlefs_conf_t conf = {
    .base_path = CONFIG_EXAMPLE_MOUNT_POINT,
    .partition_label = "storage",
    .format_if_mount_failed = true,
    .dont_mount = false,
  };

  esp_err_t err = esp_vfs_littlefs_register(&conf);

  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
  }

  return err;
#endif

#if CONFIG_EXAMPLE_BACKEND_SD
  esp_vfs_fat_mount_config_t mountConfig = {
    .max_files = 4,
    .format_if_mount_failed = true,
    .allocation_unit_size = 16384,
  };
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)CONFIG_EXAMPLE_SD_CLK;
  slot.cmd = (gpio_num_t)CONFIG_EXAMPLE_SD_CMD;
  slot.d0 = (gpio_num_t)CONFIG_EXAMPLE_SD_D0;
  slot.d1 = (gpio_num_t)CONFIG_EXAMPLE_SD_D1;
  slot.d2 = (gpio_num_t)CONFIG_EXAMPLE_SD_D2;
  slot.d3 = (gpio_num_t)CONFIG_EXAMPLE_SD_D3;
  slot.width = CONFIG_EXAMPLE_SD_BUS_WIDTH;

  sdmmc_card_t *card = NULL;
  esp_err_t err = esp_vfs_fat_sdmmc_mount(CONFIG_EXAMPLE_MOUNT_POINT, &host,
                                          &slot, &mountConfig, &card);

  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
  }

  return err;
#endif

  return ESP_ERR_NOT_SUPPORTED;
}

static void unmountStorage(void)
{
#if CONFIG_EXAMPLE_BACKEND_LITTLEFS
  esp_vfs_littlefs_unregister("storage");
#endif

#if CONFIG_EXAMPLE_BACKEND_SD
  esp_vfs_fat_sdcard_unmount(CONFIG_EXAMPLE_MOUNT_POINT, NULL);
#endif
}

//-- esp_err_to_name() prints a bare "ERROR" for any code outside the
//-- generated IDF lookup table, which covers all custom
//-- RING_FILE_STORE_ERR_* codes. Map them explicitly so the serial log
//-- stays readable.
static const char *rfsErrName(esp_err_t err)
{
  switch (err)
  {
    case RING_FILE_STORE_ERR_RING_EMPTY:
      return "RING_EMPTY";
    case RING_FILE_STORE_ERR_KEY_NOT_FOUND:
      return "KEY_NOT_FOUND";
    case RING_FILE_STORE_ERR_DUPLICATE_KEY:
      return "DUPLICATE_KEY";
    case RING_FILE_STORE_ERR_ITER_END:
      return "ITER_END";
    case RING_FILE_STORE_ERR_ITER_INVALIDATED:
      return "ITER_INVALIDATED";
    case RING_FILE_STORE_ERR_SCHEMA_MISMATCH:
      return "SCHEMA_MISMATCH";
    case RING_FILE_STORE_ERR_FORMAT:
      return "FORMAT";
    case RING_FILE_STORE_ERR_CORRUPT:
      return "CORRUPT";
    case RING_FILE_STORE_ERR_READONLY:
      return "READONLY";
    case RING_FILE_STORE_ERR_IO:
      return "IO";
    case RING_FILE_STORE_ERR_RECOVERY_REQUIRED:
      return "RECOVERY_REQUIRED";
    case RING_FILE_STORE_ERR_IO_FAULTED:
      return "IO_FAULTED";
    case RING_FILE_STORE_ERR_STORAGE_FULL:
      return "STORAGE_FULL";
    case RING_FILE_STORE_ERR_ALREADY_EXISTS:
      return "ALREADY_EXISTS";
    case RING_FILE_STORE_ERR_BUSY:
      return "BUSY";
    default:
      return esp_err_to_name(err);
  }
}

static void mkRecord(meterRecord_t *rec, uint64_t hour, uint32_t energy,
                     uint16_t errors)
{
  //-- Initialize every byte including padding before handing it over.
  memset(rec, 0, sizeof(*rec));
  rec->timestamp = hour;
  rec->energyWh = energy;
  rec->errorCount = errors;
}

static void buildRingPath(char *out, size_t size)
{
  snprintf(out, size, "%s/%s", CONFIG_EXAMPLE_MOUNT_POINT,
           CONFIG_EXAMPLE_RING_FILE);
}

static ringFileConfig_t baseConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = RING_CAPACITY;
  cfg.schemaId = SCHEMA_ID;
  cfg.schemaVersion = SCHEMA_VERSION;
  cfg.recordSize = sizeof(meterRecord_t);
  cfg.key.keyOffset = (uint32_t)offsetof(meterRecord_t, timestamp);
  cfg.key.keyType = RING_FILE_KEY_U64;
  cfg.key.keySize = (uint16_t)sizeof(uint64_t);
  //-- 0: all byte positions of the key are significant. demoKeyMaskRing()
  //-- below shows a masked variant of the same timestamp key.
  cfg.key.keyPositionMask = 0;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileOpenConfig_t openConfig(bool readOnly)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = SCHEMA_ID;
  cfg.expectedSchemaVersion = SCHEMA_VERSION;
  cfg.expectedRecordSize = sizeof(meterRecord_t);
  cfg.readOnly = readOnly;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static void logRecord(const char *label, const meterRecord_t *rec)
{
  ESP_LOGI(TAG, "%s: key=%llu energy=%lu errors=%u", label,
           (unsigned long long)rec->timestamp, (unsigned long)rec->energyWh,
           (unsigned)rec->errorCount);
}

//-- One dump line: label, physical slot number and the record key. A
//-- staged record (buffered mode, not yet on storage) shows slot=staged.
static void logRecordSlot(const char *label, ringFileHandle_t *handle,
                          ringFileIterator_t *iter, const meterRecord_t *rec)
{
  uint32_t slot = RING_FILE_SLOT_STAGED;
  char line[48];

  ringFileIteratorGetSlot(handle, iter, &slot);

  if (slot == RING_FILE_SLOT_STAGED)
  {
    snprintf(line, sizeof(line), "%s slot=staged", label);
  }
  else
  {
    snprintf(line, sizeof(line), "%s slot=%lu", label, (unsigned long)slot);
  }

  ESP_LOGI(TAG, "%s key=%llu energy=%lu errors=%u", line,
           (unsigned long long)rec->timestamp, (unsigned long)rec->energyWh,
           (unsigned)rec->errorCount);
}

//-- Dump every active record from oldest to newest. Called after each
//-- demonstration step so the full ring contents are visible at each
//-- stage of the test.
static void dumpRing(ringFileHandle_t *handle, const char *step)
{
  ringFileIterator_t iter;
  meterRecord_t rec;
  ringFileInfo_t info;

  //-- Feeding the watchdog here covers every demonstration step, because
  //-- a dump follows each mutating call.
  esp_task_wdt_reset();

  if (ringFileGetInfo(handle, &info) == ESP_OK)
  {
    ESP_LOGI(TAG, "ring dump after %s: count=%lu capacity=%lu", step,
             (unsigned long)info.count, (unsigned long)info.capacity);
  }

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_OLDEST, NULL, &rec) == ESP_OK)
  {
    logRecordSlot("  [oldest>", handle, &iter, &rec);

    while (ringFileIteratorNext(handle, &iter, &rec) == ESP_OK)
    {
      logRecordSlot("  [       >", handle, &iter, &rec);
    }
  }
  else
  {
    ESP_LOGI(TAG, "  (empty)");
  }
}

//-- Walk the whole ring forward and backward and log what is seen.
static void demoIteration(ringFileHandle_t *handle)
{
  ringFileIterator_t iter;
  meterRecord_t rec;

  ESP_LOGI(TAG, "-- forward (oldest to newest)");

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_OLDEST, NULL, &rec) == ESP_OK)
  {
    logRecord("next", &rec);

    while (ringFileIteratorNext(handle, &iter, &rec) == ESP_OK)
    {
      logRecord("next", &rec);
    }
  }

  ESP_LOGI(TAG, "-- backward (newest to oldest)");

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_NEWEST, NULL, &rec) == ESP_OK)
  {
    logRecord("prev", &rec);

    while (ringFileIteratorPrevious(handle, &iter, &rec) == ESP_OK)
    {
      logRecord("prev", &rec);
    }
  }
}

static void demoRingOperations(ringFileHandle_t *handle)
{
  meterRecord_t rec, out;

  //-- Wraparound with out-of-order keys on a capacity 10 ring: insert
  //-- 100, 300, 200, 50, 400 then 450..650 (fills the ring) and 700, 750
  //-- which evict exactly the two oldest (100 and 300).
  mkRecord(&rec, 100, 1000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 100");
  mkRecord(&rec, 300, 3000, 1);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 300");
  mkRecord(&rec, 200, 2000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 200");
  mkRecord(&rec, 50, 500, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 50");
  mkRecord(&rec, 400, 4000, 2);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 400");

  static const uint64_t moreKeys[] = {450, 500, 550, 600, 650};

  for (size_t i = 0; i < sizeof(moreKeys) / sizeof(moreKeys[0]); i++)
  {
    mkRecord(&rec, moreKeys[i], (uint32_t)(moreKeys[i] * 10), 0);
    ESP_ERROR_CHECK(ringFileAdd(handle, &rec));

    char step[48];
    snprintf(step, sizeof(step), "add %llu", (unsigned long long)moreKeys[i]);

    if (moreKeys[i] == 650)
    {
      strncat(step, " (ring now full)", sizeof(step) - strlen(step) - 1);
    }

    dumpRing(handle, step);
  }

  mkRecord(&rec, 700, 7000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 700 (evicts oldest 100)");
  mkRecord(&rec, 750, 7500, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 750 (evicts oldest 300)");

  //-- Yield for the idle task (task watchdog) after the burst of adds.
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "count=%lu capacity=%lu recordSize=%lu storage=%llu",
           (unsigned long)info.count, (unsigned long)info.capacity,
           (unsigned long)info.recordSize, (unsigned long long)info.storageSize);

  //-- Keyed lookups.
  uint64_t key = 200;
  ESP_ERROR_CHECK(ringFileReadByKey(handle, &key, &out));
  logRecord("read 200", &out);
  key = 100;
  ESP_LOGI(TAG, "read 100 (evicted): %s",
           rfsErrName(ringFileReadByKey(handle, &key, &out)));
  key = 700;
  ESP_ERROR_CHECK(ringFileReadByKey(handle, &key, &out));
  logRecord("read 700", &out);

  //-- Edit that preserves the insertion position: 400 keeps its place
  //-- between 50 and 450 in the dumps below.
  uint64_t hour = 400;
  mkRecord(&rec, 400, 4000, 7);
  ESP_ERROR_CHECK(ringFileUpdateByKey(handle, &hour, &rec));
  dumpRing(handle, "update 400 (errors=7, position preserved)");

  demoIteration(handle);
}

//-- Practical update-or-add pattern: a logger revises the current hourly
//-- record in RAM and commits only at the hourly boundary. In immediate
//-- mode ringFileUpsert() is one committed, synchronized operation; the
//-- RAM staging here avoids writing on every meter tick.
static void demoHourlyLogger(ringFileHandle_t *handle)
{
  meterRecord_t staged;
  bool hasStaged = false;
  uint64_t hour = 1000;
  uint32_t energy = 5000;

  //-- Meter ticks within the hour: accumulate in RAM only.
  for (int tick = 0; tick < 10; tick++)
  {
    if (!hasStaged)
    {
      mkRecord(&staged, hour, energy + (uint32_t)tick, 0);
      hasStaged = true;
    }
    else
    {
      staged.energyWh = energy + (uint32_t)tick;
    }
  }

  //-- Hour boundary: commit the revision (update because hour exists).
  ESP_ERROR_CHECK(ringFileUpsert(handle, &staged));
  logRecord("hourly commit", &staged);
  dumpRing(handle, "hourly commit (update 1000)");
  hasStaged = false;

  //-- Next hour begins: a new key appends instead.
  hour += 100;
  mkRecord(&staged, hour, energy + 50, 1);
  ESP_ERROR_CHECK(ringFileUpsert(handle, &staged));
  logRecord("new hour appended", &staged);
  dumpRing(handle, "new hour appended (1100)");

  //-- Yield for the idle task (task watchdog) before the close below.
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));
}

//-- Buffered variant of the hourly logger. Revisions to the current hour
//-- are staged in RAM inside the component; repeated updates to the same
//-- active record coalesce and only ringFileSync() (or close) commits.
//-- Power-loss window: everything staged since the last sync is lost.
static void demoHourlyLoggerBuffered(const char *path)
{
  ringFileOpenConfig_t cfg = openConfig(false);
  cfg.persistMode = RING_FILE_PERSIST_BUFFERED;
  cfg.maxPendingRecords = 8;
  ringFileHandle_t *handle = NULL;

  ESP_ERROR_CHECK(ringFileOpen(path, &cfg, &handle));

  meterRecord_t staged;
  uint64_t hour = 2000;
  uint64_t key = hour;

  //-- Several revisions of the current hour: staged and coalesced.
  for (int tick = 0; tick < 5; tick++)
  {
    mkRecord(&staged, hour, 7000 + (uint32_t)tick, 0);
    ESP_ERROR_CHECK(ringFileUpsert(handle, &staged));
  }

  dumpRing(handle, "buffered staged upserts (hour 2000, not yet synced)");

  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "buffered staged: pending=%lu dirty=%d (writes still pending)",
           (unsigned long)info.pendingCount, (int)info.dirty);

  //-- Reads on the same handle observe the staged state.
  meterRecord_t out;
  ESP_ERROR_CHECK(ringFileReadByKey(handle, &key, &out));
  logRecord("staged read", &out);

  //-- Hour boundary: explicit commit.
  ESP_ERROR_CHECK(ringFileSync(handle));
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "after sync: pending=%lu dirty=%d",
           (unsigned long)info.pendingCount, (int)info.dirty);

  dumpRing(handle, "buffered ringFileSync");
  ringFileClose(handle);
}

static void demoResizeAndValidate(const char *path)
{
  ringFileHandle_t *handle = NULL;
  ringFileOpenConfig_t ocfg = openConfig(false);
  ESP_ERROR_CHECK(ringFileOpen(path, &ocfg, &handle));

  ESP_LOGI(TAG, "-- resize capacity %u -> 14", (unsigned)RING_CAPACITY);
  ESP_ERROR_CHECK(ringFileResize(handle, 14));
  dumpRing(handle, "resize 10 -> 14");
  meterRecord_t rec;
  mkRecord(&rec, 900, 9000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, &rec));
  dumpRing(handle, "add 900");

  ESP_LOGI(TAG, "-- shrink back to 3 (newest retained)");
  ESP_ERROR_CHECK(ringFileResize(handle, 3));
  dumpRing(handle, "shrink 14 -> 3");
  demoIteration(handle);
  ringFileClose(handle);

  ringFileValidation_t validation;
  ESP_ERROR_CHECK(ringFileValidate(path, &validation));
  ESP_LOGI(TAG, "validate: header=%d committed=%lu badSlots=%lu strays=%d",
           (int)validation.headerValid, (unsigned long)validation.committedCount,
           (unsigned long)validation.badSlotCount, (int)validation.strayResizeFiles);
}

//-- Ring whose u64 timestamp key uses a key position mask: only the byte
//-- positions whose mask bit is set take part in key comparisons. With
//-- mask 0xFC the low bytes 0 and 1 (the seconds bits below 2^16) are
//-- insignificant, so samples that differ only there address the same
//-- record; byte 2 and above still decide key equality. The mask is
//-- stored in the file header and reported by ringFileGetInfo().
static void demoKeyMaskRing(void)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/keymask.rfs", CONFIG_EXAMPLE_MOUNT_POINT);

  if (ringFileBreakStaleLock(path) == ESP_OK)
  {
    ESP_LOGW(TAG, "cleared stale lock (keymask ring) from an interrupted run");
  }

  ringFileConfig_t cfg = baseConfig();
  cfg.maxRecords = 4;
  cfg.key.keyPositionMask = 0x00000000000000FCULL;

  ringFileHandle_t *handle = NULL;
  esp_err_t err = ringFileCreate(path, &cfg, &handle);

  if (err == RING_FILE_STORE_ERR_ALREADY_EXISTS)
  {
    ringFileOpenConfig_t ocfg = openConfig(false);
    ESP_ERROR_CHECK(ringFileOpen(path, &ocfg, &handle));
  }
  else
  {
    ESP_ERROR_CHECK(err);
  }

  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "key mask ring: mask=0x%016llx (byte positions 2-7 "
           "significant, 0-1 ignored)",
           (unsigned long long)info.keyPositionMask);

  meterRecord_t rec;
  uint64_t base = 1765000000ULL;

  //-- Start from a known-empty ring so every boot logs the same scenario.
  ESP_ERROR_CHECK(ringFileClear(handle));

  mkRecord(&rec, base, 1000, 0);
  err = ringFileAdd(handle, &rec);
  ESP_LOGI(TAG, "  add key=%llu: %s", (unsigned long long)base,
           rfsErrName(err));

  uint64_t sameKey = base + 47;
  mkRecord(&rec, sameKey, 1047, 0);
  err = ringFileAdd(handle, &rec);
  ESP_LOGI(TAG, "  add key=%llu (only bytes 0-1 differ): %s",
           (unsigned long long)sameKey, rfsErrName(err));

  uint64_t nextKey = base + 65536;
  mkRecord(&rec, nextKey, 66536, 0);
  err = ringFileAdd(handle, &rec);
  ESP_LOGI(TAG, "  add key=%llu (byte 2 differs): %s",
           (unsigned long long)nextKey, rfsErrName(err));

  meterRecord_t out;
  uint64_t probeKey = base + 9;
  err = ringFileReadByKey(handle, &probeKey, &out);
  ESP_LOGI(TAG, "  read key=%llu (byte 0 ignored): %s",
           (unsigned long long)probeKey, rfsErrName(err));

  if (err == ESP_OK)
  {
    logRecord("  read result", &out);
  }

  dumpRing(handle, "key mask scenario");
  esp_task_wdt_reset();
  ringFileClose(handle);
}

void app_main(void)
{
  ESP_LOGI(TAG, "ring_file_store example starting (backend: %s)",
#if CONFIG_EXAMPLE_BACKEND_LITTLEFS
           "LittleFS"
#else
           "SD card (FATFS)"
#endif
          );

  ESP_ERROR_CHECK(mountStorage());

  //-- Diagnostics: subscribe this task to the task watchdog so a stall
  //-- inside a LittleFS/flash operation (which the idle-task checks no
  //-- longer catch) produces a backtrace of the stuck task instead of a
  //-- silent hang. The task is fed after every demonstration step.
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  //-- Yield so the idle task can feed the task watchdog between the
  //-- flash-heavy LittleFS phases of the demonstration.
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  char path[128];
  buildRingPath(path, sizeof(path));
  ringFileConfig_t cfg = baseConfig();
  ringFileHandle_t *handle = NULL;

  //-- A previous run that died before ringFileClose() (panic, reset, power
  //-- loss) leaves its lock file behind, which would block every later open
  //-- with BUSY forever. Right after a reset no live handle can exist, so
  //-- taking over the stale lock is safe here.
  if (ringFileBreakStaleLock(path) == ESP_OK)
  {
    ESP_LOGW(TAG, "cleared stale lock file left by an interrupted run");
  }

  esp_err_t err = ringFileCreate(path, &cfg, &handle);

  ringFileOpenConfig_t ocfg = openConfig(false);

  if (err == RING_FILE_STORE_ERR_ALREADY_EXISTS)
  {
    ESP_LOGI(TAG, "reopening existing ring %s", path);
    ESP_ERROR_CHECK(ringFileOpen(path, &ocfg, &handle));
    dumpRing(handle, "open existing ring");
  }
  else
  {
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "created ring %s", path);
    dumpRing(handle, "create");
    demoRingOperations(handle);
  }

  demoHourlyLogger(handle);
  esp_task_wdt_reset();
  ringFileClose(handle);
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  //-- Reopen read-write after a restart and verify the state survived.
  ESP_ERROR_CHECK(ringFileOpen(path, &ocfg, &handle));
  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "after reopen: count=%lu capacity=%lu",
           (unsigned long)info.count, (unsigned long)info.capacity);
  dumpRing(handle, "reopen after close");
  demoIteration(handle);
  esp_task_wdt_reset();
  ringFileClose(handle);
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  demoHourlyLoggerBuffered(path);
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));
  demoResizeAndValidate(path);
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));
  demoKeyMaskRing();
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  //-- Read-only open never writes and can serve concurrent readers.
  ringFileHandle_t *ro = NULL;
  ringFileOpenConfig_t roCfg = openConfig(true);
  ESP_ERROR_CHECK(ringFileOpen(path, &roCfg, &ro));
  meterRecord_t out;
  ESP_ERROR_CHECK(ringFileReadNewest(ro, &out));
  logRecord("newest (read-only handle)", &out);
  dumpRing(ro, "read-only open");
  esp_task_wdt_reset();
  ringFileClose(ro);
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
  unmountStorage();
  ESP_LOGI(TAG, "example finished");
}
