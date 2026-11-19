```
FileVersion: [2] 19-11-2026 - 09:06

ringFileStore - repository README: project overview, usage and API documentation.
Copyright (C) 2026 Willem Aandewiel (mrWheel)
```

# ringFileStore — persistent ring files for ESP-IDF

ESP32-S3 / ESP-IDF 6.0.2 project containing the reusable
[`ring_file_store`](components/ring_file_store/README.md) component and a
local example application that exercises the full API against LittleFS or
an SD card (FATFS).

The root project is the VS Code example; the publishable package is the
component directory only.

## Quick start

The component targets **ESP32-S3** (primary) and classic **ESP32** (e.g.
WROVER, used for the current hardware tests). Select the matching target:

```sh
idf.py set-target esp32s3   # or: idf.py set-target esp32
idf.py build
```

For an ESP32-WROVER: PSRAM is enabled through `sdkconfig.defaults.esp32`
(`CONFIG_SPIRAM=y`; the component itself needs only bounded RAM per
handle), and the SD example pin defaults follow the classic ESP32 SDMMC
wiring (CLK 14, CMD 15, D0 2, D1 4, D2 12, D3 13) — change them under
menuconfig → *Ring file store example* → *SD card pins* to match your
wiring.

The example mounts the LittleFS partition `storage` (default) or an SD
card (menuconfig → *Ring file store example* → *Storage backend*) and runs:
create, reopen, wraparound with out-of-order keys, keyed lookup, edit
(hourly update-or-add), iteration in both directions, resize, validation,
buffered-mode staging, a read-only handle and a **timestamp ring file**
(TIMEBUCKET addressing: component-derived hour/day/week/month buckets from
a `uint64_t timeStamp` field, DSMRlogger32-style).

Flash manually (uploads are handled by the developer):

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## Project layout

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv` | ESP-IDF project, target and custom partition table (LittleFS partition `storage`) |
| `main/` | Example application: mounts the backend and demonstrates the component |
| `main/Kconfig.projbuild` | Backend selection, mount point, SD pin configuration |
| `components/ring_file_store/` | The component: public header, implementation, README, manifest |
| `components/ring_file_store/test/` | Host-side automated test suite (failure injection, crash-state checks) |
| `projectPrompt.md` | Implementation specification |
| `progressLog.md` | Project log of completed work and attempts |
| `PUBLISHING.md` | ESP Component Registry release steps |

## Tests

Host-side suite (no hardware; compiles the component against POSIX shims
and injects failures at every write/sync/rename boundary):

```sh
components/ring_file_store/test/run_tests.sh
```

Coverage: empty/single-slot rings, duplicates, all key types and key
comparison rules, out-of-order insertion and wraparound, updates, iterator
invalidation, schema mismatch, reopen, corruption detection, resizing
(including after wraparound, capacity one, shrink retention), full/clear
with pending buffered data, volume-full handling, write/sync counting
(unchanged updates never write; buffered edits coalesce), interrupted
mutations/renames leaving a complete previous-or-new state and stale-lock
takeover after an interrupted run, timestamp helpers (string↔unix,
leap-year and boundary validation, Monday-aligned week and ISO week
numbers) and TIMEBUCKET integration (not-newer rejection, same-bucket
overwrite, backfill window, bucket-ordered iteration and eviction,
logical `bucketId % capacity` slot, format-version-2 compatibility).

Hardware: the LittleFS backend is exercised by the example build only;
no backend was tested on physical hardware in this session (uploads are
manual). The SD/FATFS path is compile-verified and pin-configurable.

## Open in VS Code

1. Use the Espressif ESP-IDF extension and select your ESP-IDF 6.0.2
   installation.
2. Select ESP32-S3 as the target and use the extension's Build command.

The archive includes no machine-specific SDK paths, serial ports or SD pin
assignments.

## logging

Maintain a project log (`progressLog.md`) of completed tasks and all
implementation attempts, including unsuccessful ones. Record the date and
time of each attempt in "DD-MM-YYYY - HH:MM" format, along with what was
tried, the result, and any errors or findings.

Review the log before retrying an approach. Retry a failed approach only
when a relevant change gives you a concrete reason to expect a different
result. Record what changed before retrying. If approaches A and B both
failed, move on to a new approach unless something has changed that
addresses the earlier failure.

## Registry packaging

The package starts in `components/ring_file_store/`, which has its own
`idf_component.yml`. The root project remains the local VS Code example
and is outside the component package. See [PUBLISHING.md](PUBLISHING.md).

## API documentation

Detailed reference for every public call of
[ring_file_store.h](components/ring_file_store/include/ring_file_store.h),
with worked, chained code examples. The binary format, commit protocol,
durability and wear figures stay in the
[component README](components/ring_file_store/README.md); this section is
about calling each function correctly.

### How the examples are chained

Every example below belongs to exactly one of three fixed setups.
**Example 1** of any function always operates on Setup A, **Example 2**
always on Setup B and **Example 3** always on Setup C — so all "Example 1"
snippets together form one continuous program, and the same holds for the
"Example 2" and "Example 3" series. Some functions show only two
snippets where that covers the behaviour.

Examples are grouped by function, not by execution order. Each snippet
lists the ring state it expects on entry, and the state produced by its
**Expected result** is exactly the state the next snippet of the same
setup expects. Ring contents are always written `[oldest … newest]` in
insertion order — never in key order.

| | Setup A — meter | Setup B — monthly | Setup C — sensor |
| --- | --- | --- | --- |
| Path | `/storage/meter.rfs` | `/storage/monthly.rfs` | `/storage/sensor.rfs` |
| Record | `meterRecord_t` (16 B) | `monthRecord_t` (16 B) | `sensorRecord_t` (12 B) |
| Key field | `uint64_t timestamp` | `char month[8]`, e.g. `"202610"` | `uint8_t deviceId[4]` |
| Key type | `RING_FILE_KEY_U64` | `RING_FILE_KEY_STRING` (keySize 8) | `RING_FILE_KEY_BYTES` (keySize 4) |
| Capacity | 3 → resized to 2 | 12 → resized to 24 | 32 → resized to 2 |
| Persistence | immediate | immediate | buffered (`maxPendingRecords` 8) |
| Schema | `0x4D455431` ("MET1") v1 | `0x4D4F4E31` ("MON1") v1 | `0x53454E31` ("SEN1") v1 |
| Logical size | 96 + 4×36 = 240 B | 96 + 13×36 = 564 B | 96 + 33×32 = 1152 B |

The application owns the filesystem mount (see *Quick start*); the
examples assume an already-mounted LittleFS or FATFS volume and that the
three paths live on it. `ESP_ERROR_CHECK()` aborts on any error — snippets
that demonstrate an error path capture the value in an `esp_err_t` instead.

### Shared declarations used by every example

```c
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "ring_file_store.h"

//-- Setup A — hourly meter history keyed by the hourly timestamp.
typedef struct
{
  uint64_t timestamp;      //-- key: hour epoch seconds
  uint32_t energyWh;       //-- cumulative energy at hour end
  uint16_t errorCount;     //-- errors seen during the hour
  uint16_t pad;            //-- explicit padding, always initialized
} meterRecord_t;             //-- sizeof == 16

//-- Setup B — monthly totals keyed by a "YYYYMM" string, e.g. "202610".
typedef struct
{
  char month[8];           //-- key: up to 6 digits + terminator
  uint32_t totalWh;        //-- total energy for the month
  uint16_t outages;        //-- power-loss events in the month
  uint16_t pad;            //-- explicit padding, always initialized
} monthRecord_t;             //-- sizeof == 16

//-- Setup C — one record per sensor, keyed by a 4-byte device id.
typedef struct
{
  uint8_t deviceId[4];     //-- key: raw binary device id
  uint32_t tempMilliC;     //-- temperature in milli-degrees C
  uint16_t humidity;       //-- relative humidity in percent
  uint16_t pad;            //-- explicit padding, always initialized
} sensorRecord_t;            //-- sizeof == 12

#define METER_PATH  "/storage/meter.rfs"
#define MONTH_PATH  "/storage/monthly.rfs"
#define SENS_PATH   "/storage/sensor.rfs"

//-- Builders: every byte (including padding) is initialized before a
//-- record is handed to the component — unchanged-update detection
//-- compares all recordSize bytes.
static void mkMeter(meterRecord_t *rec, uint64_t hour, uint32_t energy,
                    uint16_t errors)
{
  memset(rec, 0, sizeof(*rec));
  rec->timestamp = hour;
  rec->energyWh = energy;
  rec->errorCount = errors;
}

static void mkMonth(monthRecord_t *rec, const char *yyyymm, uint32_t totalWh,
                    uint16_t outages)
{
  //-- memset clears the key first, snprintf always terminates: the
  //-- terminator is guaranteed within the 8 key bytes.
  memset(rec, 0, sizeof(*rec));
  snprintf(rec->month, sizeof(rec->month), "%s", yyyymm);
  rec->totalWh = totalWh;
  rec->outages = outages;
}

static void mkSensor(sensorRecord_t *rec, uint8_t a, uint8_t b, uint8_t c,
                     uint8_t d, uint32_t tempMilliC, uint16_t humidity)
{
  memset(rec, 0, sizeof(*rec));
  rec->deviceId[0] = a;
  rec->deviceId[1] = b;
  rec->deviceId[2] = c;
  rec->deviceId[3] = d;
  rec->tempMilliC = tempMilliC;
  rec->humidity = humidity;
}

static ringFileConfig_t meterConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 3;
  cfg.schemaId = 0x4D455431u;
  cfg.schemaVersion = 1u;
  cfg.recordSize = (uint16_t)sizeof(meterRecord_t);
  cfg.key.keyOffset = (uint32_t)offsetof(meterRecord_t, timestamp);
  cfg.key.keyType = RING_FILE_KEY_U64;
  cfg.key.keySize = (uint16_t)sizeof(uint64_t);
  cfg.key.keyPositionMask = 0;   //-- 0: all key bytes significant
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileConfig_t monthConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 12;
  cfg.schemaId = 0x4D4F4E31u;
  cfg.schemaVersion = 1u;
  cfg.recordSize = (uint16_t)sizeof(monthRecord_t);
  cfg.key.keyOffset = (uint32_t)offsetof(monthRecord_t, month);
  cfg.key.keyType = RING_FILE_KEY_STRING;
  cfg.key.keySize = (uint16_t)sizeof(((monthRecord_t *)0)->month);
  cfg.key.keyPositionMask = 0;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileConfig_t sensorConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 32;
  cfg.schemaId = 0x53454E31u;
  cfg.schemaVersion = 1u;
  cfg.recordSize = (uint16_t)sizeof(sensorRecord_t);
  cfg.key.keyOffset = (uint32_t)offsetof(sensorRecord_t, deviceId);
  cfg.key.keyType = RING_FILE_KEY_BYTES;
  cfg.key.keySize = (uint16_t)sizeof(((sensorRecord_t *)0)->deviceId);
  cfg.key.keyPositionMask = 0;
  cfg.persistMode = RING_FILE_PERSIST_BUFFERED;
  cfg.maxPendingRecords = 8;
  return cfg;
}

static ringFileOpenConfig_t openCfg(uint32_t schemaId, uint32_t schemaVersion,
                                    uint32_t recordSize, bool readOnly)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = schemaId;
  cfg.expectedSchemaVersion = schemaVersion;
  cfg.expectedRecordSize = recordSize;
  cfg.readOnly = readOnly;
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}
```

### ringFileCreate

```c
esp_err_t ringFileCreate(const char *path, const ringFileConfig_t *config,
                         ringFileHandle_t **outHandle);
```

Creates an empty ring **exclusively** and returns it open read-write. It
never recreates or truncates an existing file, and `*outHandle` is set
only on `ESP_OK`.

**Example 1 — Setup A (numeric u64 key, capacity 3):**

```c
ringFileConfig_t cfg = meterConfig();
ringFileHandle_t *meter = NULL;

ESP_ERROR_CHECK(ringFileCreate(METER_PATH, &cfg, &meter));
```

*Expected result:* `ESP_OK`, `meter` open read-write on an empty ring
`[ ]`. The 96-byte header is written and synced at create; slots extend
lazily as records are added, and `ringFileGetInfo()` reports the logical
`storageSize` 96 + (3+1)×36 = 240 bytes. Creating the same path again
returns `RING_FILE_STORE_ERR_ALREADY_EXISTS` and leaves the file and the
handle pointer untouched. Invalid configurations (`maxRecords` 0,
`recordSize` 0, key outside the record, `keySize` mismatching the integer
width, or a key position mask setting bits at/ beyond `keySize`) return
`ESP_ERR_INVALID_ARG` before any file is touched.

**Example 2 — Setup B (string key "202610", capacity 12):**

```c
ringFileConfig_t cfg = monthConfig();
ringFileHandle_t *monthly = NULL;

ESP_ERROR_CHECK(ringFileCreate(MONTH_PATH, &cfg, &monthly));
```

*Expected result:* `ESP_OK`, empty ring `[ ]`, capacity 12. The key
descriptor (offset 0, `RING_FILE_KEY_STRING`, keySize 8, mask 0) is
persisted in the header: every later `ringFileOpen()` must expect schema
id `0x4D4F4E31`, version 1 and `recordSize` 16, or it fails with
`RING_FILE_STORE_ERR_SCHEMA_MISMATCH`.

**Example 3 — Setup C (byte-array key, capacity 32, buffered mode):**

```c
ringFileConfig_t cfg = sensorConfig();   //-- buffered, maxPendingRecords 8
ringFileHandle_t *sensors = NULL;

ESP_ERROR_CHECK(ringFileCreate(SENS_PATH, &cfg, &sensors));
```

*Expected result:* `ESP_OK`, empty ring `[ ]`, capacity 32, record size
12 (logical `storageSize` 96 + 33×32 = 1152 bytes). `RING_FILE_PERSIST_BUFFERED`
makes later mutations stage in RAM instead of writing per call — create
itself always commits its header immediately.

### ringFileOpen

```c
esp_err_t ringFileOpen(const char *path, const ringFileOpenConfig_t *openConfig,
                       ringFileHandle_t **outHandle);
```

Opens an existing ring; capacity and schema come **from the file** and
must match the `expected*` fields. A read-write open takes the exclusive
lock file `<path>.lck` and may perform recovery writes; a read-only open
never writes, takes no lock and returns
`RING_FILE_STORE_ERR_RECOVERY_REQUIRED` instead of repairing.

**Example 1 — Setup A (reopen after the create session):**

```c
ringFileClose(meter);   //-- end of the create session

//-- A wrong schema expectation is rejected, handle not returned.
ringFileOpenConfig_t badCfg = openCfg(0x4D455431u, 1u, 20, false);
ringFileHandle_t *bad = NULL;
esp_err_t err = ringFileOpen(METER_PATH, &badCfg, &bad);

//-- The matching expectation reopens the persisted ring.
ringFileOpenConfig_t ocfg = openCfg(0x4D455431u, 1u, sizeof(meterRecord_t),
                                    false);
ringFileHandle_t *meter = NULL;
ESP_ERROR_CHECK(ringFileOpen(METER_PATH, &ocfg, &meter));
```

*Expected result:* `err == RING_FILE_STORE_ERR_SCHEMA_MISMATCH` for the
16→20 byte record-size lie (`bad` stays `NULL`); the second open returns
`ESP_OK` and the ring continues exactly where the previous session left
it. (The mismatch call runs while no handle is open: a writable handle
holds `<path>.lck`, so opening the same path read-write twice returns
`RING_FILE_STORE_ERR_BUSY`.)

**Example 2 — Setup B (read-only reader, state `[202610, 202611*, 202609]`):**

```c
ringFileOpenConfig_t roCfg = openCfg(0x4D4F4E31u, 1u, sizeof(monthRecord_t),
                                     true);
ringFileHandle_t *ro = NULL;
ESP_ERROR_CHECK(ringFileOpen(MONTH_PATH, &roCfg, &ro));

monthRecord_t out;
ESP_ERROR_CHECK(ringFileReadNewest(ro, &out));

monthRecord_t rec;
mkMonth(&rec, "202612", 0, 0);
esp_err_t err = ringFileAdd(ro, &rec);
```

*Expected result:* `ringFileReadNewest` → `ESP_OK` with `out.month ==
"202609"` (newest by insertion — see *ringFileUpsert*). The add attempt
on the read-only handle → `RING_FILE_STORE_ERR_READONLY`, ring unchanged.
The read-only open wrote nothing, created no lock and never needs one;
several read-only handles may coexist. Close it with `ringFileClose()`
like any other handle.

**Example 3 — Setup C (read-write reopen in buffered mode):**

```c
ringFileOpenConfig_t ocfg = openCfg(0x53454E31u, 1u, sizeof(sensorRecord_t),
                                    false);
ocfg.persistMode = RING_FILE_PERSIST_BUFFERED;
ocfg.maxPendingRecords = 8;
ringFileHandle_t *sensors = NULL;

ESP_ERROR_CHECK(ringFileOpen(SENS_PATH, &ocfg, &sensors));
```

*Expected result:* `ESP_OK`. Persistence mode is a per-handle property,
not stored in the file — the reopen explicitly selects buffered mode
again. The staged buffer starts empty (`pendingCount` 0, `dirty` false)
and the handle sees the last committed state; anything staged but
unsynchronized before a previous reset was lost (see *ringFileSync*).

### ringFileClose

```c
void ringFileClose(ringFileHandle_t *handle);
```

Commits pending staged data, closes the file, releases the lock and frees
the handle. The pointer is invalid afterwards; `ringFileClose(NULL)` is
safe. A clean handle performs **no write and no sync**.

**Example 1 — Setup A (clean close after the last mutation):**

```c
ringFileClose(meter);
```

*Expected result:* the handle is clean (immediate mode already committed
and synced every mutation), so this returns with zero component writes
and zero fsyncs, releases `<path>.lck` and frees all handle memory.

**Example 2 — Setup B (close at the end of the read-write session):**

```c
ringFileClose(monthly);
```

*Expected result:* void, no writes (everything was already committed).
Exactly the records of the last committed mutation are what the later
read-only session in *ringFileOpen Example 2* sees. The read-only handle
closes the same way and never writes either.

**Example 3 — Setup C (close flushes the staged epoch after *ringFileClear*):**

```c
ringFileClose(sensors);
```

*Expected result:* close commits the still-pending clear epoch change as
one 96-byte header write + fsync, then releases the lock and frees the
handle. `ringFileClose()` returns void: a flush that fails at close is
logged (`close commit failed`), and the storage always keeps a complete
previous-or-new committed state — call `ringFileSync()` explicitly when
you must observe a commit failure.

### ringFileAdd

```c
esp_err_t ringFileAdd(ringFileHandle_t *handle, const void *record);
```

Extracts the key from the record itself and appends it as newest.
A duplicate active key is rejected **without touching any record**; at
capacity exactly the oldest record is evicted. Evicted keys may be added
again.

**Example 1 — Setup A (out-of-order keys, duplicate, wraparound):**

```c
meterRecord_t rec;

mkMeter(&rec, 100, 1000, 0);
ESP_ERROR_CHECK(ringFileAdd(meter, &rec));
mkMeter(&rec, 300, 3000, 1);
ESP_ERROR_CHECK(ringFileAdd(meter, &rec));
mkMeter(&rec, 200, 2000, 0);
ESP_ERROR_CHECK(ringFileAdd(meter, &rec));

mkMeter(&rec, 200, 9999, 9);
esp_err_t err = ringFileAdd(meter, &rec);   //-- key already active

mkMeter(&rec, 50, 500, 0);
ESP_ERROR_CHECK(ringFileAdd(meter, &rec));  //-- ring was full
```

*Expected result:* the first three → `ESP_OK`, ring becomes
`[100, 300, 200]` — insertion order, not key order; the gap between 100,
300 and 200 costs no storage. The duplicate add of 200 →
`RING_FILE_STORE_ERR_DUPLICATE_KEY` and changes nothing (200 still holds
energy 2000). The add of 50 → `ESP_OK`: at capacity exactly the oldest
(100) is evicted → `[300, 200, 50]`. Each `ESP_OK` add in immediate mode
is one slot image + one fsync.

**Example 2 — Setup B (string keys, empty/full buffer rules):**

```c
monthRecord_t rec;

mkMonth(&rec, "202610", 312400, 2);
ESP_ERROR_CHECK(ringFileAdd(monthly, &rec));
mkMonth(&rec, "202611", 180900, 1);
ESP_ERROR_CHECK(ringFileAdd(monthly, &rec));

mkMonth(&rec, "202610", 0, 0);
esp_err_t err = ringFileAdd(monthly, &rec);   //-- duplicate key

memset(&rec, '2', sizeof(rec));               //-- "22222222": no terminator
err = ringFileAdd(monthly, &rec);
```

*Expected result:* first two → `ESP_OK`, ring `[202610, 202611]`. The
second "202610" → `RING_FILE_STORE_ERR_DUPLICATE_KEY` — string keys
compare case-sensitively byte-by-byte up to the first terminator. The
unterminated key (8 `'2'` bytes, no `'\0'` inside `keySize`) →
`ESP_ERR_INVALID_ARG`; unterminated string keys are always rejected.
Also note: `"202610"` ≠ `"202610 "` (a space before the terminator is a
different key), while `"202610"` matches a buffer whose bytes after the
first terminator hold anything.

**Example 3 — Setup C (byte-array keys, staged without touching storage):**

```c
sensorRecord_t rec;

mkSensor(&rec, 0, 0, 10, 1, 21500, 55);
ESP_ERROR_CHECK(ringFileAdd(sensors, &rec));
mkSensor(&rec, 0, 0, 10, 2, 21800, 52);
ESP_ERROR_CHECK(ringFileAdd(sensors, &rec));
mkSensor(&rec, 0, 0, 10, 3, 22000, 48);
ESP_ERROR_CHECK(ringFileAdd(sensors, &rec));
mkSensor(&rec, 0, 0, 11, 7, 19500, 61);
ESP_ERROR_CHECK(ringFileAdd(sensors, &rec));

mkSensor(&rec, 0, 0, 10, 1, 0, 0);
esp_err_t err = ringFileAdd(sensors, &rec);
```

*Expected result:* the first four → `ESP_OK`, ring
`[{0,0,10,1}, {0,0,10,2}, {0,0,10,3}, {0,0,11,7}]`. Nothing is on storage
yet: `ringFileGetInfo()` reports `pendingCount` 4, `dirty` true, so a
power cut right now loses all four. The duplicate →
`RING_FILE_STORE_ERR_DUPLICATE_KEY` even before the first flush — the
duplicate check covers staged records. Byte-array keys compare by exact
byte equality: `{0,0,10,1}` ≠ `{0,0,1,10}` (positions matter). With
capacity 32 the 33rd add would evict exactly the oldest record.

### ringFileReadByKey

```c
esp_err_t ringFileReadByKey(ringFileHandle_t *handle, const void *key,
                            void *outRecord);
```

Copies the active record carrying the key into the caller's struct. On
any error `outRecord` is not written — check the return value first.

**Example 1 — Setup A (state `[300, 200, 50]`):**

```c
meterRecord_t out;
uint64_t key = 200;

ESP_ERROR_CHECK(ringFileReadByKey(meter, &key, &out));
//-- out: timestamp=200 energyWh=2000 errorCount=0

key = 100;
esp_err_t err = ringFileReadByKey(meter, &key, &out);
```

*Expected result:* first → `ESP_OK` with a byte-exact copy of record 200.
Key 100 was evicted by the wraparound add → `RING_FILE_STORE_ERR_KEY_NOT_FOUND`.
The key argument is a caller-owned buffer of `keySize` bytes carrying the
raw little-endian field bytes — on the (little-endian) ESP32, `&key` on a
`uint64_t` is exactly that.

**Example 2 — Setup B (string key buffer contract):**

```c
monthRecord_t out;
char key[8];

memset(key, 0, sizeof(key));
snprintf(key, sizeof(key), "%s", "202610");
ESP_ERROR_CHECK(ringFileReadByKey(monthly, key, &out));
//-- out.month="202610" totalWh=312400 outages=2
```

*Expected result:* `ESP_OK` with the October record. The buffer is
deliberately the full `keySize` (8) bytes: the contract requires
caller-owned key buffers of **at least keySize bytes**, and the component
scans all `keySize` bytes for the terminator. A key that is inactive
("202609") → `RING_FILE_STORE_ERR_KEY_NOT_FOUND`; a key buffer without a
terminator within 8 bytes → `ESP_ERR_INVALID_ARG`.

**Example 3 — Setup C (reads observe staged data; state: 4 staged):**

```c
sensorRecord_t out;
const uint8_t key[4] = {0, 0, 10, 2};

ESP_ERROR_CHECK(ringFileReadByKey(sensors, key, &out));
//-- out: tempMilliC=21800 humidity=52  (staged, not yet on storage)
```

*Expected result:* `ESP_OK` with the **staged** image: reads through the
same handle observe buffered changes that were never written. An
inactive key (`{0,0,9,9}`) → `RING_FILE_STORE_ERR_KEY_NOT_FOUND`. Other
handles do **not** see staged data — the overlay is per handle.

### ringFileUpdateByKey

```c
esp_err_t ringFileUpdateByKey(ringFileHandle_t *handle, const void *key,
                              const void *record);
```

Replaces the complete record of an active key **preserving its insertion
position**. The replacement's embedded key must equal the lookup key. A
replacement identical to the current record produces no write at all.

**Example 1 — Setup A (state `[300, 200, 50]`):**

```c
meterRecord_t rec;
uint64_t key = 200;

mkMeter(&rec, 200, 2000, 7);
ESP_ERROR_CHECK(ringFileUpdateByKey(meter, &key, &rec));
//-- ring: [300, 200*, 50]  -- same position, errorCount now 7

mkMeter(&rec, 201, 2000, 7);
esp_err_t err = ringFileUpdateByKey(meter, &key, &rec);

mkMeter(&rec, 200, 2000, 7);
ESP_ERROR_CHECK(ringFileUpdateByKey(meter, &key, &rec));
```

*Expected result:* update → `ESP_OK`; 200 keeps its place between 300 and
50 (only the revision counter in the slot header advances). Second call:
the replacement carries key 201 while the lookup key is 200 →
`ESP_ERR_INVALID_ARG`, nothing changes. Third call is byte-identical to
the current record (padding included) → `ESP_OK` with **zero** writes and
syncs. Updating an inactive key (e.g. 100) →
`RING_FILE_STORE_ERR_KEY_NOT_FOUND` — use `ringFileUpsert()` for
update-or-add.

**Example 2 — Setup B (revising the current month, state `[202610, 202611]`):**

```c
monthRecord_t rec;
char key[8];

memset(key, 0, sizeof(key));
snprintf(key, sizeof(key), "%s", "202611");
mkMonth(&rec, "202611", 198500, 1);
ESP_ERROR_CHECK(ringFileUpdateByKey(monthly, key, &rec));
```

*Expected result:* `ESP_OK`; `totalWh` goes 180900 → 198500 and "202611"
stays newest at position 2 — the key is unchanged, so insertion order is
untouched. Note the key buffer again: full 8 bytes, terminated.

**Example 3 — Setup C (buffered edits coalesce, state: 4 staged):**

```c
sensorRecord_t rec;
const uint8_t key[4] = {0, 0, 10, 3};

mkSensor(&rec, 0, 0, 10, 3, 22100, 49);
ESP_ERROR_CHECK(ringFileUpdateByKey(sensors, key, &rec));
```

*Expected result:* `ESP_OK`; the staged image of `{0,0,10,3}` is replaced
(22000/48 → 22100/49) and **coalesced** with that record's staged add:
`pendingCount` stays 4, and only one slot image for this key will be
written at the next flush. Reads through this handle see 22100
immediately.

### ringFileUpsert

```c
esp_err_t ringFileUpsert(ringFileHandle_t *handle, const void *record);
```

Atomic update-or-add under the handle lock — the hourly-logger pattern:
update in place when the key is active, otherwise append as newest.
The key is taken from the record itself.

**Example 1 — Setup A (state `[300, 200*, 50]`):**

```c
meterRecord_t rec;

mkMeter(&rec, 200, 2050, 7);
ESP_ERROR_CHECK(ringFileUpsert(meter, &rec));
//-- ring: [300, 200**, 50]   (key active  -> updated in place)

mkMeter(&rec, 450, 4500, 0);
ESP_ERROR_CHECK(ringFileUpsert(meter, &rec));
//-- ring: [200**, 50, 450]   (key inactive -> appended, 300 evicted)
```

*Expected result:* first → `ESP_OK`, 200 revised again (energy 2050)
still sitting between 300 and 50. Second → `ESP_OK`, 450 was inactive so
it is appended as newest and, with the ring full, exactly the oldest
(300) is evicted. Each `ESP_OK` in immediate mode is one committed,
synchronized write. Lookup and mutation happen under the handle lock, so
no other user of this handle can slip an add between them.

**Example 2 — Setup B (backfilling an older month):**

```c
monthRecord_t rec;

mkMonth(&rec, "202609", 412300, 3);
ESP_ERROR_CHECK(ringFileUpsert(monthly, &rec));
```

*Expected result:* `ESP_OK`; "202609" is inactive → appended as the
**newest** record even though it is the oldest calendar month:
`[202610, 202611*, 202609]`. Upsert never sorts by key — "newest" always
means insertion order.

**Example 3 — Setup C (staged append through upsert):**

```c
sensorRecord_t rec;

mkSensor(&rec, 0, 0, 10, 4, 23100, 45);
ESP_ERROR_CHECK(ringFileUpsert(sensors, &rec));
```

*Expected result:* `ESP_OK`; `{0,0,10,4}` was inactive → staged append,
`pendingCount` 5. Had the key been active (committed or staged), the same
call would have replaced its staged image instead of appending.

### ringFileReadOldest / ringFileReadNewest

```c
esp_err_t ringFileReadOldest(ringFileHandle_t *handle, void *outRecord);
esp_err_t ringFileReadNewest(ringFileHandle_t *handle, void *outRecord);
```

Copy the first / last record in **insertion order**. Empty ring →
`RING_FILE_STORE_ERR_RING_EMPTY`.

**Example 1 — Setup A (state `[200**, 50, 450]`):**

```c
meterRecord_t oldest, newest;

ESP_ERROR_CHECK(ringFileReadOldest(meter, &oldest));
//-- oldest: key 200 (energyWh 2050)
ESP_ERROR_CHECK(ringFileReadNewest(meter, &newest));
//-- newest: key 450
```

*Expected result:* `ESP_OK` on both: oldest is 200 — the oldest record
*surviving*, the earlier 100 and 300 were evicted; newest is the upserted
450. This is not key order: 200 < 450 here only by coincidence.

**Example 2 — Setup B (state `[202610, 202611*, 202609]`):**

```c
monthRecord_t oldest, newest;

ESP_ERROR_CHECK(ringFileReadOldest(monthly, &oldest));
//-- oldest.month == "202610"  (first add)
ESP_ERROR_CHECK(ringFileReadNewest(monthly, &newest));
//-- newest.month == "202609"  (the backfilled month!)
```

*Expected result:* `ESP_OK` on both, and the point of the example: the
"newest" month is the one added last, not the highest label —
insertion order decides. On an empty ring (right after
*ringFileClear*) both calls return `RING_FILE_STORE_ERR_RING_EMPTY`.

### ringFileIteratorBegin / Next / Previous / GetSlot

```c
esp_err_t ringFileIteratorBegin(ringFileHandle_t *handle,
                                ringFileIterator_t *iterator,
                                ringFileIterStart_t start, const void *key,
                                void *outRecord);
esp_err_t ringFileIteratorNext(ringFileHandle_t *handle,
                               ringFileIterator_t *iterator, void *outRecord);
esp_err_t ringFileIteratorPrevious(ringFileHandle_t *handle,
                                   ringFileIterator_t *iterator, void *outRecord);
esp_err_t ringFileIteratorGetSlot(ringFileHandle_t *handle,
                                  const ringFileIterator_t *iterator,
                                  uint32_t *outSlot);
```

Caller-owned cursors (a stack variable is fine, treat it as opaque after
`begin`). Start at `RING_FILE_ITER_OLDEST`, `RING_FILE_ITER_NEWEST` or an
exact `RING_FILE_ITER_KEY`. Every concurrent traversal needs its own
cursor. A successful mutation on the handle invalidates existing cursors.

**Example 1 — Setup A (forward, backward, by key, invalidation; state `[200**, 50, 450]`):**

```c
ringFileIterator_t it;
meterRecord_t rec;
uint32_t slot;

ESP_ERROR_CHECK(ringFileIteratorBegin(meter, &it, RING_FILE_ITER_OLDEST,
                                      NULL, &rec));
while (ringFileIteratorNext(meter, &it, &rec) == ESP_OK)
{
  ringFileIteratorGetSlot(meter, &it, &slot);
}
```

*Expected result:* `begin` → `ESP_OK` with the oldest record (200**);
`next` → 50, `next` → 450, then `next` →
`RING_FILE_STORE_ERR_ITER_END`. `ringFileIteratorGetSlot` reports a
physical slot index `0 … 3` (capacity+1 slots rotate — do not depend on
the exact number). Backward: `begin` with `RING_FILE_ITER_NEWEST` → 450,
then `previous` → 50 → 200** → `ITER_END`. By key: `begin` with
`RING_FILE_ITER_KEY` and `&key` (say 50) starts at that record; with an
inactive key → `RING_FILE_STORE_ERR_KEY_NOT_FOUND`; `begin` OLDEST/NEWEST
on an empty ring → `RING_FILE_STORE_ERR_RING_EMPTY`; `ITER_KEY` with
`key == NULL` → `ESP_ERR_INVALID_ARG`.

```c
//-- Invalidation: a mutation kills the live cursor.
ringFileIteratorBegin(meter, &it, RING_FILE_ITER_OLDEST, NULL, &rec);
mkMeter(&rec, 600, 6000, 0);
ESP_ERROR_CHECK(ringFileAdd(meter, &rec));
esp_err_t err = ringFileIteratorNext(meter, &it, &rec);
//-- err == RING_FILE_STORE_ERR_ITER_INVALIDATED; ring now [50, 450, 600]
```

*Expected result:* `err == RING_FILE_STORE_ERR_ITER_INVALIDATED` — the
successful add evicted the oldest record, so every cursor on this handle
must be restarted with `begin`. (This add also advances Setup A to
`[50, 450, 600]`, the state the later examples use.)

**Example 2 — Setup B (backward walk from newest; state `[202610, 202611*, 202609]`):**

```c
ringFileIterator_t it;
monthRecord_t rec;

ESP_ERROR_CHECK(ringFileIteratorBegin(monthly, &it, RING_FILE_ITER_NEWEST,
                                      NULL, &rec));
//-- rec.month == "202609"
while (ringFileIteratorPrevious(monthly, &it, &rec) == ESP_OK)
{
  //-- "202611", then "202610"
}
```

*Expected result:* `begin` → "202609", `previous` → "202611", `previous`
→ "202610", then `previous` → `RING_FILE_STORE_ERR_ITER_END`. The cursor
may live on the stack; copy it only after `begin` succeeded.

**Example 3 — Setup C (staged records report a synthetic slot; state: 5 staged):**

```c
ringFileIterator_t it;
sensorRecord_t rec;
uint32_t slot;

ESP_ERROR_CHECK(ringFileIteratorBegin(sensors, &it, RING_FILE_ITER_OLDEST,
                                      NULL, &rec));
while (ringFileIteratorNext(sensors, &it, &rec) == ESP_OK)
{
  ringFileIteratorGetSlot(sensors, &it, &slot);
}
```

*Expected result:* the walk visits the five staged records in insertion
order `{0,0,10,1}`, `{0,0,10,2}`, `{0,0,10,3}`, `{0,0,11,7}`,
`{0,0,10,4}`. Before the first `ringFileSync()` every `GetSlot` reports
`RING_FILE_SLOT_STAGED` (`0xFFFFFFFFu`): those records exist only in the
RAM buffer. After the *ringFileSync* example the same walk reports real
slot indices `0 … 32`. `GetSlot` after an `ITER_END` still reports the
slot of the last successfully returned record.

### ringFileResize

```c
esp_err_t ringFileResize(ringFileHandle_t *handle, uint32_t newCapacity);
```

Changes capacity preserving relative insertion order; on shrink the
**newest** records that fit are retained. `newCapacity` ≥ 1 (0 →
`ESP_ERR_INVALID_ARG`; equal to current → `ESP_OK` no-op). Staged data is
committed first. The resize builds a candidate file `.rfsnew`, verifies it,
then switches with two renames — the committed ring survives every
recoverable failure (see the component README, *Resize*). Temporary free
space needed: header + (retained+1) × slotSize; volume full →
`RING_FILE_STORE_ERR_STORAGE_FULL` (or `..._ERR_IO`) with the original
untouched.

**Example 1 — Setup A (shrink 3 → 2; state `[50, 450, 600]`):**

```c
ESP_ERROR_CHECK(ringFileResize(meter, 2));
```

*Expected result:* `ESP_OK` → `[450, 600]`: the two newest survive in
their relative order, 50 is dropped. Capacity 2 is persisted
(`storageSize` becomes 96 + 3×36 = 204 bytes) and all cursors are
invalidated (it is a mutation).

**Example 2 — Setup B (grow 12 → 24; state `[202610, 202611*, 202609]`):**

```c
ESP_ERROR_CHECK(ringFileResize(monthly, 24));
```

*Expected result:* `ESP_OK`; all three records kept in order, capacity
now 24 (`storageSize` 96 + 25×36 = 996 bytes) — 21 more months fit before
the first eviction ever happens.

**Example 3 — Setup C (large ring shrunk hard; state: 5 records, capacity 32):**

```c
ESP_ERROR_CHECK(ringFileResize(sensors, 2));
```

*Expected result:* `ESP_OK`. Resize first commits any staged data (a
no-op here after the *ringFileSync* example), then keeps the newest two
records → `[{0,0,11,7}, {0,0,10,4}]` with capacity 2. The handle stays
usable for further adds; the ring fills at 2.

### ringFileGetInfo

```c
esp_err_t ringFileGetInfo(ringFileHandle_t *handle, ringFileInfo_t *outInfo);
```

Snapshot: `count` (including staged), `capacity` (from the **file**),
`recordSize`, `schemaId`, `schemaVersion`, `storageSize` (logical),
`pendingCount`, `dirty`, `keyPositionMask`.

**Example 1 — Setup A (state `[50, 450, 600]`):**

```c
ringFileInfo_t info;
ESP_ERROR_CHECK(ringFileGetInfo(meter, &info));
```

*Expected result:* `ESP_OK` with `count` 3, `capacity` 3, `recordSize`
16, `schemaId` 0x4D455431, `schemaVersion` 1, `storageSize` 240,
`pendingCount` 0, `dirty` false (immediate mode never stays dirty),
`keyPositionMask` 0.

**Example 2 — Setup B (3 records in a 12-ring):**

```c
ringFileInfo_t info;
ESP_ERROR_CHECK(ringFileGetInfo(monthly, &info));
```

*Expected result:* `ESP_OK` with `count` 3, `capacity` 12, `recordSize`
16, `storageSize` 564, `pendingCount` 0, `dirty` false. (After the
*ringFileResize* example `capacity` reads 24 and `storageSize` 996.)

**Example 3 — Setup C (buffered with staged data; state: 5 staged):**

```c
ringFileInfo_t info;
ESP_ERROR_CHECK(ringFileGetInfo(sensors, &info));
```

*Expected result:* `ESP_OK` with `count` 5, `capacity` 32, `recordSize`
12, `storageSize` 1152, **`pendingCount` 5, `dirty` true** — five records
are acknowledged but only staged in RAM. `count` already includes them;
`pendingCount`/`dirty` tell you a flush is still outstanding. A power cut
now loses all five, so schedule *ringFileSync()* below your acceptable
loss window.

### ringFileSync

```c
esp_err_t ringFileSync(ringFileHandle_t *handle);
```

Commits pending buffered changes and synchronizes through the backend.
A clean handle performs no write and no sync.

**Example 1 — Setup A (immediate mode):**

```c
ESP_ERROR_CHECK(ringFileSync(meter));
```

*Expected result:* `ESP_OK` with zero writes — nothing was pending (every
mutation already committed itself), and clean syncs never touch storage.
Safe to call periodically from any loop.

**Example 2 — Setup C (commit the staged buffer; state: 5 staged):**

```c
ESP_ERROR_CHECK(ringFileSync(sensors));
```

*Expected result:* `ESP_OK`; the five staged records are written as slot
images (20-byte slot header + record each) and fsynced. Afterwards
`ringFileGetInfo` shows `pendingCount` 0, `dirty` false, and iterators
report real slot indices instead of `RING_FILE_SLOT_STAGED`. Before this
call a power cut lost all five samples; after it nothing is lost. The
staged age is unbounded unless the application schedules such syncs —
the component commits on its own only when the 8-slot buffer is full.

### ringFileValidate

```c
esp_err_t ringFileValidate(const char *path, ringFileValidation_t *outValidation);
```

Stand-alone **read-only** integrity check by path (no handle, never
writes). Missing file → `ESP_ERR_NOT_FOUND`; otherwise `ESP_OK` with the
result struct — a damaged header is *reported*, not returned as an error.

**Example 1 — Setup A (state `[450, 600]`, capacity 2 after the shrink):**

```c
ringFileValidation_t v;
ESP_ERROR_CHECK(ringFileValidate(METER_PATH, &v));
```

*Expected result:* `ESP_OK`; `headerValid` true, `metadataIssue`
`RING_FILE_META_OK`, `capacity` 2, `recordSize` 16, `slotCount` 3
(capacity+1 positions examined), `badSlotCount` 0, `committedCount` 2,
`strayResizeFiles` false. A torn slot image (interrupted write) shows up
as `badSlotCount` > 0 while the derived committed state stays complete;
a damaged header returns `headerValid` false with `metadataIssue`
`BAD_MAGIC` / `BAD_FORMAT_VERSION` / `BAD_CHECKSUM` / `BAD_GEOMETRY` /
`SHORT_FILE`. Safe to call from a monitoring task at any time.

**Example 2 — Setup B (state: capacity 24 after the grow):**

```c
ringFileValidation_t v;
ESP_ERROR_CHECK(ringFileValidate(MONTH_PATH, &v));
```

*Expected result:* `ESP_OK`; `capacity` 24, `recordSize` 16, `slotCount`
25, `committedCount` 3, `badSlotCount` 0. `strayResizeFiles` is true only
while `.rfsnew`/`.rfsold` leftovers from an interrupted resize exist —
the next read-write open cleans them up (or recovers from them), so it is
a diagnostic, not an error.

### ringFileClear

```c
esp_err_t ringFileClear(ringFileHandle_t *handle);
```

Explicitly discards all records, retaining schema and capacity: one
committed header write advances the epoch and hides every record. Staged
data is dropped.

**Example 1 — Setup A (state `[450, 600]`, capacity 2):**

```c
ESP_ERROR_CHECK(ringFileClear(meter));

meterRecord_t out;
esp_err_t err = ringFileReadOldest(meter, &out);
```

*Expected result:* `ESP_OK`; ring `[ ]`, `ringFileGetInfo` reports
`count` 0 with `capacity` still 2 and the schema unchanged. The follow-up
read → `RING_FILE_STORE_ERR_RING_EMPTY`. In immediate mode the epoch bump
is committed and synced inside the call — a single 96-byte header write,
no records rewritten. Old slot contents become reusable scratch, so even
the cleared keys themselves can be added again.

**Example 2 — Setup C (clear drops staged data; state: 2 committed, buffered):**

```c
sensorRecord_t rec;
mkSensor(&rec, 0, 0, 20, 1, 18000, 70);
ESP_ERROR_CHECK(ringFileAdd(sensors, &rec));   //-- staged, pendingCount 1

ESP_ERROR_CHECK(ringFileClear(sensors));
```

*Expected result:* the add → `ESP_OK` (staged); the clear → `ESP_OK`
drops the staged add (`pendingCount` back to 0) and hides the committed
records (`count` 0). In buffered mode the epoch change itself is
committed at the next flush (*ringFileSync* or *ringFileClose*): until
then a power cut can resurface the pre-clear state — a complete
previous-or-new state, never a mix. Capacity 2 and schema are retained.

### ringFileBreakStaleLock

```c
esp_err_t ringFileBreakStaleLock(const char *path);
```

Removes the `<path>.lck` lock file left behind by a handle that never
reached `ringFileClose()` (panic, reset, power loss). Call it **only
right after a reset, before the first open**, when no live handle can
exist on that path. It never touches the ring file itself.

**Example 1 — Setup A (startup recovery, mirrors `main/main.c`):**

```c
if (ringFileBreakStaleLock(METER_PATH) == ESP_OK)
{
  ESP_LOGW(TAG, "cleared stale lock from an interrupted run");
}

ringFileHandle_t *meter = NULL;
ESP_ERROR_CHECK(ringFileOpen(METER_PATH, &ocfg, &meter));
```

*Expected result:* without this, the orphaned lock makes every writable
open fail with `RING_FILE_STORE_ERR_BUSY` forever. `ESP_OK` means a lock
was removed; `ESP_ERR_NOT_FOUND` (the normal boot case) means nothing was
there. Calling it while a handle is open breaks that handle's protection
— its close would then remove somebody else's lock.

**Example 2 — Setup B (normal case after a clean session):**

```c
esp_err_t err = ringFileBreakStaleLock(MONTH_PATH);
```

*Expected result:* `ESP_ERR_NOT_FOUND` — the previous session released
its lock at `ringFileClose()`, so there is nothing to break.

### Timestamp ring files (TIMEBUCKET addressing)

Time-series files can let the component derive the key from a
`uint64_t timeStamp` field (unix seconds of the local wall clock treated
as if UTC, DSMRlogger32-compatible) instead of a caller-chosen key. The
granularity is fixed at create time; many timestamps map to the same
**bucketId**, and they all address the same record.

```c
typedef struct
{
  uint64_t timeStamp;   //-- key is derived from this field
  uint32_t energyWh;
  uint16_t errorCount;
  uint16_t pad;
} hourRecord_t;

ringFileConfig_t cfg = {0};
cfg.maxRecords = 24;                        //-- 24 hourly buckets
cfg.schemaId = 0x484F5531;
cfg.schemaVersion = 1;
cfg.recordSize = sizeof(hourRecord_t);
cfg.addressing = RING_FILE_ADDRESSING_TIMEBUCKET;
cfg.granularity = RING_FILE_GRANULARITY_HOUR;   //-- DAY, WEEK or MONTH
cfg.timestampOffset = offsetof(hourRecord_t, timeStamp);
//-- cfg.key is ignored: the key becomes the computed bucketId.

ringFileHandle_t *hourly = NULL;
ESP_ERROR_CHECK(ringFileCreate("/storage/hourly.rfs", &cfg, &hourly));
```

`ringFileOpen()` must repeat `expectedAddressing`,
`expectedGranularity` and `expectedTimestampOffset` or the stored schema
is rejected with `RING_FILE_STORE_ERR_SCHEMA_MISMATCH`. Order, eviction
and resize retention follow **bucketId**, not insertion seq.

**Example — the hourly logger pattern:**

```c
uint64_t ts;
ESP_ERROR_CHECK(ringFileTimeStampStringToUnix("20261023141500", &ts));

hourRecord_t rec = {0};
rec.timeStamp = ts;
rec.energyWh = 5432;

//-- New hour: appends. Same hour again: overwrites (same bucket).
ESP_ERROR_CHECK(ringFileUpsert(hourly, &rec));

//-- Clock rewind or gap fill through add() is refused:
rec.timeStamp = ts - 3600;                  //-- previous hour
esp_err_t err = ringFileAdd(hourly, &rec);  //-- ..._ERR_NOT_NEWER

//-- Backfill that previous hour explicitly (lost measurement):
ESP_ERROR_CHECK(ringFileEdit(hourly, &rec));

ringFileIterator_t iter;
memset(&iter, 0, sizeof(iter));
ESP_ERROR_CHECK(ringFileIteratorBegin(hourly, &iter, RING_FILE_ITER_NEWEST,
                                      NULL, &rec));
uint32_t slot;
ESP_ERROR_CHECK(ringFileIteratorGetSlot(hourly, &iter, &slot));  //-- bucketId % 24
```

Granularity → bucketId: HOUR `ts / 3600`; DAY `ts / 86400`; WEEK
`(ts + 259200) / 604800` (boundary **Monday 00:00**, ISO-8601); MONTH
`(EEYY − 1) × 12 + MM` (DSMRlogger32 formula, 2026-10 → 24310).

### ringFileEdit

```c
esp_err_t ringFileEdit(ringFileHandle_t *handle, const void *record);
```

TIMEBUCKET mode only (otherwise `ESP_ERR_INVALID_STATE`): backfill/edit
when measurements for one or more buckets were lost. Updates the bucket
when active (any `timeStamp`, unlike `ringFileAdd`); otherwise inserts it
when inside the retained window
`[newestBucket − capacity + 1, newestBucket]` — an empty ring accepts any
bucket. Outside the window: `RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE`,
nothing changes. At capacity the insert evicts exactly the oldest bucket.

### ringFileTimeStampStringToUnix / ringFileUnixToTimeStampString

```c
esp_err_t ringFileTimeStampStringToUnix(const char *timeStamp, uint64_t *outUnixSeconds);
esp_err_t ringFileUnixToTimeStampString(uint64_t unixSeconds, char outTimeStamp[15]);
```

Convert between unix seconds and the 14-digit `"EEYYMMDDHHMMSS"` string
(century fixed `20`, years 2000–2099). Parse validation: exactly 14
digits, month 01–12, day valid for the month including leap years, hour
00–23, minute/second 00–59; violations are `ESP_ERR_INVALID_ARG`.
`"20000101000000"` ↔ `946684800`. `outTimeStamp` receives 14 digits plus
a NUL; values outside 2000–2099 are `ESP_ERR_INVALID_ARG`.

### ringFileUnixToBucketId / ringFileUnixToIsoWeek

```c
esp_err_t ringFileUnixToBucketId(uint64_t unixSeconds, ringFileGranularity_t granularity,
                                 uint64_t *outBucketId);
esp_err_t ringFileUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear, uint32_t *outIsoWeek);
```

`ringFileUnixToBucketId()` computes the key value the ring uses (also
usable to build the 8-byte key for `ringFileReadByKey()` /
`ringFileUpdateByKey()`). `ringFileUnixToIsoWeek()` returns the ISO-8601
week-numbering year and week (weeks start Monday; week 1 contains the
first Thursday) for display — 2025-12-29 is ISO 2026-W01. An invalid
granularity is `ESP_ERR_INVALID_ARG`.

### Result codes (quick reference)

Standard ESP-IDF codes: `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`,
`ESP_ERR_INVALID_STATE`, `ESP_ERR_NOT_FOUND`, `ESP_ERR_INVALID_SIZE`.
Custom codes share base `RING_FILE_STORE_ERR_BASE` (0x5600):

| Code | Cause |
| --- | --- |
| `..._ERR_RING_EMPTY` | readOldest/readNewest/iterator begin on a ring with no records |
| `..._ERR_KEY_NOT_FOUND` | no active record carries the key |
| `..._ERR_DUPLICATE_KEY` | add/upsert-add with an already active key |
| `..._ERR_ITER_END` | iterator reached the logical end in that direction |
| `..._ERR_ITER_INVALIDATED` | a successful mutation invalidated the cursor |
| `..._ERR_SCHEMA_MISMATCH` | stored schema id/version/record size ≠ expectation |
| `..._ERR_FORMAT` | magic, format version or geometry not understood |
| `..._ERR_CORRUPT` | integrity failure; recovery cannot determine a valid state |
| `..._ERR_READONLY` | mutation attempted through a read-only handle |
| `..._ERR_IO` | backend I/O failure (short write, fsync, rename) |
| `..._ERR_RECOVERY_REQUIRED` | read-only open found a state that needs writing to recover |
| `..._ERR_IO_FAULTED` | handle quarantined after an ambiguous I/O failure; close and reopen |
| `..._ERR_STORAGE_FULL` | resize candidate could not be created/filled (volume full) |
| `..._ERR_ALREADY_EXISTS` | create refused to overwrite an existing file |
| `..._ERR_BUSY` | another writable handle holds `<path>.lck` |
| `..._ERR_NOT_NEWER` | TIMEBUCKET add/upsert with a `timeStamp` not newer than the newest stored record (use `ringFileEdit`) |
| `..._ERR_BUCKET_OUT_OF_RANGE` | TIMEBUCKET `ringFileEdit` for a bucket outside the retained window |

`esp_err_to_name()` prints a bare `"ERROR"` for these custom codes — the
example maps them with a local `rfsErrName()` helper (see `main/main.c`).

### Buffer contract (recap)

* Record buffers are caller-owned, at least `recordSize` bytes; the
  component reads/writes exactly `recordSize` bytes and never keeps a
  pointer.
* `memset` the whole struct before filling it — unchanged-update
  detection compares all bytes, padding included.
* Key arguments are caller-owned buffers of **at least `keySize` bytes**:
  integers as raw little-endian field bytes, byte arrays exactly
  `keySize`, strings null-terminated within `keySize` (bytes after the
  terminator are ignored; an unterminated key is `ESP_ERR_INVALID_ARG`).
* Records must be pointer-free with fixed-width fields; payloads are raw
  bytes, so reopening requires the same layout, ABI and little-endian
  byte order — any change needs a new `schemaVersion`.
