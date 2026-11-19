//--
//-- FileVersion: [14] 19-11-2026 - 15:25
//--
//-- ringFileStore - Example application for the ring_file_store component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
//---------------------------------------------------------------------
//--
//-- Runs the same ring operations against either LittleFS (internal flash
//-- partition "storage") or a FATFS-formatted SD card, selected in
//-- menuconfig under "Ring file store example". The application owns
//-- mounting and unmounting; the component only sees VFS paths.
//--
//-- Every file in this example is timestamp-keyed: the component always
//-- prepends [timeStamp][usedKey] to the stored record and always returns
//-- both fields on reads; the application structs only carry the payload.
//--
//-- Demonstrated with a "YYMMDDhh" meter ring: create, reopen, wraparound,
//-- lookup by usedKey, edit (update-or-add of the current hourly record),
//-- iteration in both directions, resize, validation and information
//-- reporting, plus a buffered-mode variant of the hourly logger showing
//-- the coalesced commit. Every dump line shows the timeStamp, the
//-- usedKey and the reported bucketId (period % capacity). A second ring
//-- demonstrates the "YYMM" monthly file type (monthly periods)
//-- and a third the "YYMMDDhhmmss" per-second file type.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

//-- Caller payload structs: no key fields, only fixed-width members. The
//-- component adds timeStamp and usedKey on write and returns them on
//-- every read.
typedef struct
{
  uint32_t energyWh;       //-- cumulative energy at hour end
  uint16_t errorCount;     //-- errors during the hour
  uint16_t pad;            //-- explicit padding, always initialized
} meterRecord_t;

typedef struct
{
  uint32_t totalWh;        //-- cumulative energy at month end
  uint16_t outages;        //-- power-loss events during the month
  uint16_t pad;            //-- explicit padding, always initialized
  uint32_t peakW;          //-- highest measured power this month
} monthRecord_t;

typedef struct
{
  uint32_t tempMilliC;     //-- temperature in milli-degrees C
  uint16_t humidity;       //-- relative humidity percent
  uint16_t pad;            //-- explicit padding, always initialized
} sensorRecord_t;

//-- Read output structs: the automatic key fields, then the payload.
typedef struct
{
  ringFileKeyFields_t keys;
  meterRecord_t payload;
} meterFull_t;

typedef struct
{
  ringFileKeyFields_t keys;
  monthRecord_t payload;
} monthFull_t;

typedef struct
{
  ringFileKeyFields_t keys;
  sensorRecord_t payload;
} sensorFull_t;

#define RING_CAPACITY 10u

//-- Build a 14-digit EEYYMMDDhhmmss numeric timestamp.
static uint64_t mkTs(uint32_t year, uint32_t month, uint32_t day, uint32_t hh,
                     uint32_t mm, uint32_t ss)
{
  uint64_t v = year;
  v = v * 100u + month;
  v = v * 100u + day;
  v = v * 100u + hh;
  v = v * 100u + mm;
  v = v * 100u + ss;
  return v;
}

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
    case RING_FILE_STORE_ERR_NOT_NEWER:
      return "NOT_NEWER";
    case RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE:
      return "BUCKET_OUT_OF_RANGE";
    default:
      return esp_err_to_name(err);
  }
}

static void mkRecord(meterRecord_t *rec, uint32_t energy, uint16_t errors)
{
  //-- Initialize every byte including padding before handing it over.
  memset(rec, 0, sizeof(*rec));
  rec->energyWh = energy;
  rec->errorCount = errors;
}

static void buildRingPath(char *out, size_t size)
{
  snprintf(out, size, "%s/%s", CONFIG_EXAMPLE_MOUNT_POINT,
           CONFIG_EXAMPLE_RING_FILE);
}

//-- "YYMMDDhh" file type: hourly buckets.
static ringFileConfig_t baseConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = RING_CAPACITY;
  cfg.schemaId = RING_FILE_SCHEMA_YYMMDDHH;
  cfg.recordSize = sizeof(meterRecord_t);
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileOpenConfig_t openConfig(bool readOnly)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = RING_FILE_SCHEMA_YYMMDDHH;
  cfg.expectedRecordSize = sizeof(meterRecord_t);
  cfg.readOnly = readOnly;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

//-- Create the ring or reopen the existing one. A file left behind by an
//-- older firmware with an incompatible header/payload layout fails open
//-- with FORMAT, CORRUPT or SCHEMA_MISMATCH; the example then self-heals
//-- by dropping the old file and creating a fresh ring of the current
//-- schema. A production application should archive the data first. The
//-- stale lock of an interrupted run is cleared before opening (no live
//-- handle can exist right after a reset).
static ringFileHandle_t *openOrRecreateRing(const char *path,
                                            const ringFileConfig_t *cfg,
                                            const ringFileOpenConfig_t *ocfg)
{
  ringFileHandle_t *handle = NULL;

  if (ringFileBreakStaleLock(path) == ESP_OK)
  {
    ESP_LOGW(TAG, "cleared stale lock file for %s", path);
  }

  esp_err_t err = ringFileCreate(path, cfg, &handle);

  if (err == RING_FILE_STORE_ERR_ALREADY_EXISTS)
  {
    ESP_LOGI(TAG, "reopening existing ring %s", path);
    err = ringFileOpen(path, ocfg, &handle);

    if (err == RING_FILE_STORE_ERR_FORMAT ||
        err == RING_FILE_STORE_ERR_CORRUPT ||
        err == RING_FILE_STORE_ERR_SCHEMA_MISMATCH)
    {
      ESP_LOGW(TAG, "ring %s is from an incompatible firmware (%s), "
               "recreating", path, rfsErrName(err));
      unlink(path);
      err = ringFileCreate(path, cfg, &handle);
    }
  }

  ESP_ERROR_CHECK(err);
  return handle;
}

static void logRecord(const char *label, const meterFull_t *rec)
{
  ESP_LOGI(TAG, "%s: ts=%014llu usedKey=%llu energy=%lu errors=%u", label,
           (unsigned long long)rec->keys.timeStamp,
           (unsigned long long)rec->keys.usedKey,
           (unsigned long)rec->payload.energyWh,
           (unsigned)rec->payload.errorCount);
}

//-- One dump line: label, reported bucketId (period % capacity) and the
//-- key fields. A staged record (buffered mode, not yet on storage) shows
//-- slot=staged.
static void logRecordSlot(const char *label, ringFileHandle_t *handle,
                          ringFileIterator_t *iter, const meterFull_t *rec)
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
    snprintf(line, sizeof(line), "%s bucketId=%lu", label,
             (unsigned long)slot);
  }

  ESP_LOGI(TAG, "%s ts=%014llu usedKey=%llu energy=%lu errors=%u", line,
           (unsigned long long)rec->keys.timeStamp,
           (unsigned long long)rec->keys.usedKey,
           (unsigned long)rec->payload.energyWh,
           (unsigned)rec->payload.errorCount);
}

//-- Dump every active record from oldest to newest (bucket order). Called
//-- after each demonstration step so the full ring contents are visible at
//-- each stage of the test.
static void dumpRing(ringFileHandle_t *handle, const char *step)
{
  ringFileIterator_t iter;
  meterFull_t rec;
  ringFileInfo_t info;

  //-- Feeding the watchdog here covers every demonstration step, because
  //-- a dump follows each mutating call.
  esp_task_wdt_reset();

  if (ringFileGetInfo(handle, &info) == ESP_OK)
  {
    ESP_LOGI(TAG, "ring dump after %s: count=%lu capacity=%lu", step,
             (unsigned long)info.count, (unsigned long)info.capacity);
  }

  memset(&rec, 0, sizeof(rec));

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_OLDEST, NULL,
                            &rec) == ESP_OK)
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
  meterFull_t rec;

  ESP_LOGI(TAG, "-- forward (oldest to newest)");
  memset(&rec, 0, sizeof(rec));

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_OLDEST, NULL,
                            &rec) == ESP_OK)
  {
    logRecord("next", &rec);

    while (ringFileIteratorNext(handle, &iter, &rec) == ESP_OK)
    {
      logRecord("next", &rec);
    }
  }

  ESP_LOGI(TAG, "-- backward (newest to oldest)");
  memset(&rec, 0, sizeof(rec));

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_NEWEST, NULL,
                            &rec) == ESP_OK)
  {
    logRecord("prev", &rec);

    while (ringFileIteratorPrevious(handle, &iter, &rec) == ESP_OK)
    {
      logRecord("prev", &rec);
    }
  }
}

//-- Hourly scenario on a capacity-10 "YYMMDDhh" ring: fill 2026-10-23
//-- hours 09..18 (ring becomes full), then hours 19 and 20 evict exactly
//-- the two oldest buckets. Lookup by usedKey, an evicted key and an
//-- update that preserves the bucket position.
static void demoRingOperations(ringFileHandle_t *handle)
{
  meterRecord_t rec;
  meterFull_t out;

  //-- Start from a known-empty ring so every boot logs the same scenario.
  ESP_ERROR_CHECK(ringFileClear(handle));

  for (uint32_t hh = 9; hh <= 18; hh++)
  {
    uint64_t ts = mkTs(2026u, 10u, 23u, hh, 0u, 0u);
    mkRecord(&rec, hh * 1000u, 0);
    ESP_ERROR_CHECK(ringFileAdd(handle, ts, &rec));

    char step[48];
    snprintf(step, sizeof(step), "add hour %02u", (unsigned)hh);

    if (hh == 18)
    {
      strncat(step, " (ring now full)", sizeof(step) - strlen(step) - 1);
    }

    dumpRing(handle, step);
  }

  mkRecord(&rec, 19000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, mkTs(2026u, 10u, 23u, 19u, 0u, 0u), &rec));
  dumpRing(handle, "add hour 19 (evicts oldest 09)");
  mkRecord(&rec, 20000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, mkTs(2026u, 10u, 23u, 20u, 0u, 0u), &rec));
  dumpRing(handle, "add hour 20 (evicts oldest 10)");

  //-- Yield for the idle task (task watchdog) after the burst of adds.
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "count=%lu capacity=%lu payloadSize=%lu recordSize=%lu "
           "storage=%llu type=%s",
           (unsigned long)info.count, (unsigned long)info.capacity,
           (unsigned long)info.payloadSize, (unsigned long)info.recordSize,
           (unsigned long long)info.storageSize, info.schemaId);

  //-- Lookup by usedKey (the timestamp truncated to "YYMMDDhh").
  uint64_t usedKey = mkTs(2026u, 10u, 23u, 11u, 0u, 0u) / 10000u;
  memset(&out, 0, sizeof(out));
  ESP_ERROR_CHECK(ringFileReadByKey(handle, usedKey, &out));
  logRecord("read hour 11", &out);

  usedKey = mkTs(2026u, 10u, 23u, 9u, 0u, 0u) / 10000u;
  ESP_LOGI(TAG, "read hour 09 (evicted): %s",
           rfsErrName(ringFileReadByKey(handle, usedKey, &out)));

  usedKey = mkTs(2026u, 10u, 23u, 20u, 0u, 0u) / 10000u;
  memset(&out, 0, sizeof(out));
  ESP_ERROR_CHECK(ringFileReadByKey(handle, usedKey, &out));
  logRecord("read hour 20", &out);

  //-- Update that preserves the bucket position: hour 12 keeps its place
  //-- between 11 and 13 in the dumps below. The timestamp may carry later
  //-- minutes as long as its usedKey matches the lookup key.
  mkRecord(&rec, 12000, 7);
  ESP_ERROR_CHECK(ringFileUpdateByKey(handle,
                                      mkTs(2026u, 10u, 23u, 12u, 0u, 0u) / 10000u,
                                      mkTs(2026u, 10u, 23u, 12u, 30u, 0u),
                                      &rec));
  dumpRing(handle, "update hour 12 (errors=7, position preserved)");

  demoIteration(handle);
}

//-- Practical update-or-add pattern: a logger revises the current hourly
//-- record in RAM and commits only at the hourly boundary. In immediate
//-- mode ringFileUpsert() is one committed, synchronized operation; the
//-- RAM staging here avoids writing on every meter tick.
static void demoHourlyLogger(ringFileHandle_t *handle)
{
  meterRecord_t staged;
  meterFull_t out;

  //-- Meter ticks within hour 21: accumulate in RAM only.
  for (int tick = 0; tick < 10; tick++)
  {
    mkRecord(&staged, 21000u + (uint32_t)tick, 0);
  }

  //-- Hour boundary: append hour 21 (new bucket).
  uint64_t ts21 = mkTs(2026u, 10u, 23u, 21u, 0u, 0u);
  ESP_ERROR_CHECK(ringFileUpsert(handle, ts21, &staged));
  dumpRing(handle, "hourly append (hour 21)");

  //-- A revision inside the same hour updates that bucket in place.
  mkRecord(&staged, 21010u, 2);
  ESP_ERROR_CHECK(ringFileUpsert(handle, mkTs(2026u, 10u, 23u, 21u, 45u, 0u),
                                 &staged));
  memset(&out, 0, sizeof(out));
  ESP_ERROR_CHECK(ringFileReadByKey(handle, ts21 / 10000u, &out));
  logRecord("hour 21 revised", &out);
  dumpRing(handle, "hourly commit (update hour 21)");

  //-- Next hour begins: a new bucket appends instead.
  mkRecord(&staged, 22050, 1);
  ESP_ERROR_CHECK(ringFileUpsert(handle, mkTs(2026u, 10u, 23u, 22u, 0u, 0u),
                                 &staged));
  dumpRing(handle, "new hour appended (hour 22)");

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
  ringFileConfig_t cfg = baseConfig();
  cfg.persistMode = RING_FILE_PERSIST_BUFFERED;
  cfg.maxPendingRecords = 8;
  ringFileOpenConfig_t ocfg = openConfig(false);
  ocfg.persistMode = RING_FILE_PERSIST_BUFFERED;
  ocfg.maxPendingRecords = 8;
  ringFileHandle_t *handle = openOrRecreateRing(path, &cfg, &ocfg);

  meterRecord_t staged;
  uint64_t ts23 = mkTs(2026u, 10u, 23u, 23u, 0u, 0u);

  //-- Several revisions of the current hour: staged and coalesced.
  for (int tick = 0; tick < 5; tick++)
  {
    mkRecord(&staged, 23000u + (uint32_t)tick, 0);
    ESP_ERROR_CHECK(ringFileUpsert(handle, ts23, &staged));
  }

  dumpRing(handle, "buffered staged upserts (hour 23, not yet synced)");

  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "buffered staged: pending=%lu dirty=%d (writes still pending)",
           (unsigned long)info.pendingCount, (int)info.dirty);

  //-- Reads on the same handle observe the staged state.
  meterFull_t out;
  memset(&out, 0, sizeof(out));
  ESP_ERROR_CHECK(ringFileReadByKey(handle, ts23 / 10000u, &out));
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
  ringFileConfig_t cfg = baseConfig();
  ringFileOpenConfig_t ocfg = openConfig(false);
  ringFileHandle_t *handle = openOrRecreateRing(path, &cfg, &ocfg);

  ESP_LOGI(TAG, "-- resize capacity %u -> 14", (unsigned)RING_CAPACITY);
  ESP_ERROR_CHECK(ringFileResize(handle, 14));
  dumpRing(handle, "resize 10 -> 14");
  meterRecord_t rec;
  mkRecord(&rec, 9000, 0);
  ESP_ERROR_CHECK(ringFileAdd(handle, mkTs(2026u, 10u, 24u, 9u, 0u, 0u), &rec));
  dumpRing(handle, "add 2026-10-24 hour 09");

  ESP_LOGI(TAG, "-- shrink back to 3 (newest buckets retained)");
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

//-- "YYMM" monthly ring: periods follow the month formula
//-- (year - 1) * 12 + month. A capacity of 4 shows eviction while the
//-- year rolls over from 2026 to 2027. The month timestamps carry "00"
//-- padding below the file type.
static void demoMonthlyRing(void)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/monthly.rfs", CONFIG_EXAMPLE_MOUNT_POINT);

  if (ringFileBreakStaleLock(path) == ESP_OK)
  {
    ESP_LOGW(TAG, "cleared stale lock (monthly ring) from an interrupted run");
  }

  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 4;
  cfg.schemaId = RING_FILE_SCHEMA_YYMM;
  cfg.recordSize = sizeof(monthRecord_t);
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;

  ringFileOpenConfig_t ocfg;
  memset(&ocfg, 0, sizeof(ocfg));
  ocfg.expectedSchemaId = RING_FILE_SCHEMA_YYMM;
  ocfg.expectedRecordSize = sizeof(monthRecord_t);
  ringFileHandle_t *handle = openOrRecreateRing(path, &cfg, &ocfg);
  esp_err_t err;

  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "monthly ring: type=%s granularity=%d recordSize=%lu",
           info.schemaId, (int)info.granularity,
           (unsigned long)info.recordSize);

  monthRecord_t rec;
  monthFull_t out;
  memset(&out, 0, sizeof(out));

  //-- Start from a known-empty ring so every boot logs the same scenario.
  ESP_ERROR_CHECK(ringFileClear(handle));

  static const uint32_t months[] = {202609u, 202610u, 202611u, 202612u,
                                    202701u};

  for (size_t i = 0; i < sizeof(months) / sizeof(months[0]); i++)
  {
    uint32_t y = months[i] / 100u;
    uint32_t m = months[i] % 100u;
    uint64_t ts = mkTs(y, m, 0u, 0u, 0u, 0u);

    uint64_t usedKey = 0;
    uint64_t period = 0;
    ESP_ERROR_CHECK(ringFileTimeStampToUsedKey(ts, RING_FILE_GRANULARITY_MONTH,
                                               &usedKey));
    ESP_ERROR_CHECK(ringFileTimeStampToPeriod(ts, RING_FILE_GRANULARITY_MONTH,
                                              &period));

    memset(&rec, 0, sizeof(rec));
    rec.totalWh = months[i] * 100u;
    rec.outages = (uint16_t)(m % 3u);
    rec.peakW = 3000u + m;

    err = ringFileAdd(handle, ts, &rec);
    ESP_LOGI(TAG, "  add month %06u: usedKey=%llu period=%llu "
             "((year-1)*12+month) bucketId=%lu -> %s",
             (unsigned)months[i], (unsigned long long)usedKey,
             (unsigned long long)period,
             (unsigned long)(period % cfg.maxRecords), rfsErrName(err));
  }

  ringFileIterator_t iter;
  memset(&out, 0, sizeof(out));

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_OLDEST, NULL,
                            &out) == ESP_OK)
  {
    uint32_t slot = 0;
    ringFileIteratorGetSlot(handle, &iter, &slot);
    ESP_LOGI(TAG, "  oldest retained month: usedKey=%llu bucketId=%lu "
             "totalWh=%lu",
             (unsigned long long)out.keys.usedKey, (unsigned long)slot,
             (unsigned long)out.payload.totalWh);
  }

  memset(&out, 0, sizeof(out));
  err = ringFileReadByKey(handle, 202610u, &out);
  ESP_LOGI(TAG, "  read month 202610 (evicted by 202701): %s",
           rfsErrName(err));

  if (err == ESP_OK)
  {
    ESP_LOGI(TAG, "    ts=%014llu totalWh=%lu peakW=%lu",
             (unsigned long long)out.keys.timeStamp,
             (unsigned long)out.payload.totalWh,
             (unsigned long)out.payload.peakW);
  }

  esp_task_wdt_reset();
  ringFileClose(handle);
}

//-- "YYMMDDhhmmss" per-second ring: consecutive seconds map to
//-- consecutive buckets. Shows add on a full ring, the same-second update,
//-- NOT_NEWER rejection and ringFileEdit backfill inside the retained
//-- window. Also demonstrates the unix/timestamp helper conversions.
static void demoSensorRing(void)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/sensor.rfs", CONFIG_EXAMPLE_MOUNT_POINT);

  if (ringFileBreakStaleLock(path) == ESP_OK)
  {
    ESP_LOGW(TAG, "cleared stale lock (sensor ring) from an interrupted run");
  }

  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 8;
  cfg.schemaId = RING_FILE_SCHEMA_YYMMDDHHMMSS;
  cfg.recordSize = sizeof(sensorRecord_t);
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;

  ringFileOpenConfig_t ocfg;
  memset(&ocfg, 0, sizeof(ocfg));
  ocfg.expectedSchemaId = RING_FILE_SCHEMA_YYMMDDHHMMSS;
  ocfg.expectedRecordSize = sizeof(sensorRecord_t);
  ringFileHandle_t *handle = openOrRecreateRing(path, &cfg, &ocfg);
  esp_err_t err;

  sensorRecord_t rec;
  sensorFull_t out;

  ESP_ERROR_CHECK(ringFileClear(handle));

  uint64_t ts = mkTs(2026u, 11u, 18u, 14u, 30u, 55u);

  for (uint32_t s = 0; s < 3u; s++)
  {
    memset(&rec, 0, sizeof(rec));
    rec.tempMilliC = 21000u + s * 50u;
    rec.humidity = (uint16_t)(40u + s);
    err = ringFileAdd(handle, ts + s, &rec);
    ESP_LOGI(TAG, "  add ts=%014llu: %s", (unsigned long long)(ts + s),
             rfsErrName(err));
  }

  memset(&rec, 0, sizeof(rec));
  rec.tempMilliC = 21075u;
  rec.humidity = 41u;
  err = ringFileUpsert(handle, ts + 1u, &rec);
  ESP_LOGI(TAG, "  upsert ts=%014llu (same second -> update): %s",
           (unsigned long long)(ts + 1u), rfsErrName(err));

  err = ringFileAdd(handle, ts, &rec);
  ESP_LOGI(TAG, "  add ts=%014llu again (not newer): %s",
           (unsigned long long)ts, rfsErrName(err));

  memset(&rec, 0, sizeof(rec));
  rec.tempMilliC = 20900u;
  rec.humidity = 39u;
  uint64_t lost = ts - 1u;
  err = ringFileEdit(handle, lost, &rec);
  ESP_LOGI(TAG, "  edit ts=%014llu (backfill lost second in window): %s",
           (unsigned long long)lost, rfsErrName(err));

  memset(&out, 0, sizeof(out));
  err = ringFileReadByKey(handle, lost, &out);
  ESP_LOGI(TAG, "  read usedKey=%llu: %s", (unsigned long long)lost,
           rfsErrName(err));

  if (err == ESP_OK)
  {
    ESP_LOGI(TAG, "    ts=%014llu tempMilliC=%ld humidity=%u",
             (unsigned long long)out.keys.timeStamp,
             (long)out.payload.tempMilliC, (unsigned)out.payload.humidity);
  }

  ringFileIterator_t iter;
  memset(&out, 0, sizeof(out));

  if (ringFileIteratorBegin(handle, &iter, RING_FILE_ITER_OLDEST, NULL,
                            &out) == ESP_OK)
  {
    uint32_t slot = 0;
    ringFileIteratorGetSlot(handle, &iter, &slot);
    uint64_t period = 0;
    ESP_ERROR_CHECK(ringFileTimeStampToPeriod(out.keys.timeStamp,
                                              RING_FILE_GRANULARITY_SECOND,
                                              &period));
    ESP_LOGI(TAG, "  oldest: ts=%014llu period=%llu bucketId=%lu "
             "(period %% capacity)",
             (unsigned long long)out.keys.timeStamp,
             (unsigned long long)period, (unsigned long)slot);
  }

  //-- Helper round-trip through unix seconds.
  uint64_t unixSec = 0;
  ESP_ERROR_CHECK(ringFileTimeStampStringToUnix("20261118143055", &unixSec));
  char stamp[15];
  ESP_ERROR_CHECK(ringFileUnixToTimeStampString(unixSec, stamp));
  uint32_t isoYear = 0;
  uint32_t isoWeek = 0;
  ESP_ERROR_CHECK(ringFileUnixToIsoWeek(unixSec, &isoYear, &isoWeek));
  ESP_LOGI(TAG, "  helpers: \"20261118143055\" <-> %llu <-> \"%s\", "
           "iso=%u-W%02u",
           (unsigned long long)unixSec, stamp, (unsigned)isoYear,
           (unsigned)isoWeek);

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

  ringFileOpenConfig_t ocfg = openConfig(false);
  handle = openOrRecreateRing(path, &cfg, &ocfg);

  //-- Capacity always comes from the file, never from the create/open
  //-- config: a previous run may have left the ring resized (the resize
  //-- demo below shrinks it to 3). Normalise it here so every boot replays
  //-- the same demonstration scenario at RING_CAPACITY.
  ringFileInfo_t bootInfo;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &bootInfo));

  if (bootInfo.capacity != RING_CAPACITY)
  {
    ESP_LOGI(TAG, "stored capacity is %lu, resizing to %u for the demo",
             (unsigned long)bootInfo.capacity, (unsigned)RING_CAPACITY);
    ESP_ERROR_CHECK(ringFileResize(handle, RING_CAPACITY));
  }

  dumpRing(handle, "open");
  demoRingOperations(handle);
  demoHourlyLogger(handle);
  esp_task_wdt_reset();
  ringFileClose(handle);
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  //-- Reopen read-write after a restart and verify the state survived.
  handle = openOrRecreateRing(path, &cfg, &ocfg);
  ringFileInfo_t info;
  ESP_ERROR_CHECK(ringFileGetInfo(handle, &info));
  ESP_LOGI(TAG, "after reopen: count=%lu capacity=%lu type=%s",
           (unsigned long)info.count, (unsigned long)info.capacity,
           info.schemaId);
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
  demoMonthlyRing();
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));
  demoSensorRing();
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(10));

  //-- Read-only open never writes and can serve concurrent readers.
  ringFileHandle_t *ro = NULL;
  ringFileOpenConfig_t roCfg = openConfig(true);
  ESP_ERROR_CHECK(ringFileOpen(path, &roCfg, &ro));
  meterFull_t out;
  memset(&out, 0, sizeof(out));
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
