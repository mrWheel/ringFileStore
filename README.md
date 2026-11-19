<!--
 FileVersion: [5] 19-11-2026 - 15:25

 ringFileStore - repository README: project overview, usage and API documentation.
 Copyright (C) 2026 Willem Aandewiel (mrWheel)
 SPDX-License-Identifier: GPL-3.0-or-later
-->

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
create, reopen, wraparound with hourly buckets, lookup by `usedKey`,
backfill with `ringFileEdit` (inside/outside the retained window), the
hourly update-or-add logger, iteration in both directions, resize,
validation, buffered-mode staging, a read-only handle, a **monthly**
(`"YYMM"`) ring and a **per-second** (`"YYMMDDhhmmss"`) ring.

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
| `testLog.md` | Per-command log written by the host test suite |
| `PUBLISHING.md` | ESP Component Registry release steps |

## Tests

Host-side suite (no hardware; compiles the component against POSIX shims
and injects failures at every write/sync/rename boundary):

```sh
components/ring_file_store/test/run_tests.sh
```

Coverage: empty/single-slot rings, same-bucket overwrite (add of an
active `usedKey` updates in place), `usedKey`/period/bucketId calculation
for every file type (including the month formula and leap
years), backfill inside and outside the retained window, not-newer
rejection, bucket-ordered updates, iterator invalidation, file-type
mismatch, reopen, corruption detection, resizing (including after
wraparound, capacity one, shrink retention), full/clear with pending
buffered data, volume-full handling, write/sync counting (unchanged
updates never write; buffered edits coalesce), interrupted
mutations/renames leaving a complete previous-or-new state, stale-lock
takeover after an interrupted run, and the timestamp helpers
(string↔unix, ISO week, digit validation, `schemaId` mapping).
Every run writes [testLog.md](testLog.md) with the commands, results and
ring dumps (`usedKey`, `timeStamp` and `bucketId` per record).

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

## Timestamp key model

Every file created by this component is timestamp-keyed. The caller never
declares or describes a key field: the component **always** prepends two
fields to the stored record, and every read **returns** them.

```
stored record = [ u64 timeStamp ][ u64 usedKey ][ caller payload ... ]
                  field 1          field 2         your struct only
```

* **File type (`schemaId`)** — fixed at create, one of the strings
  `"YY"`, `"YYMM"`, `"YYMMDD"`, `"YYMMDDhh"`, `"YYMMDDhhmm"`,
  `"YYMMDDhhmmss"` (case-sensitive, lower-case `hh`/`mm`/`ss`; the
  two-digit-year form). The stored/numeric timestamp form
  always carries the century: `EEYY…`, fixed to `20` (years 2000–2099).
* **`timeStamp` (field 1)** — the full timestamp as a numeric `uint64_t`
  in `EEYYMMDDhhmmss` form (14 digits), back-filled with `00` values
  below the file type: an `"YYMMDD"`-type record for 18-11-2026 is
  stored as `20261118000000`. Digits below the file type may be zero
  padding or carry real values (e.g. `20261118000001`); they never take
  part in the key.
* **`usedKey` (field 2)** — the timestamp truncated to the file-type
  digits: with type `"YYMMDDhh"`, `20261118143055` yields
  `usedKey 2026111814` (the `YYMMDDHH` column, century
  included). All key comparisons — duplicate-bucket detection,
  `ringFileReadByKey`, update key match, keyed iteration — compare
  `usedKey`.
* **bucketId** — depending on the file type the component computes a
  period value from the timestamp; `bucketId = period % maxRecords` is
  the logical slot reported by
  `ringFileIteratorGetSlot()`.

  | schemaId | period |
  | --- | --- |
  | `"YY"` | year (e.g. 2026) |
  | `"YYMM"` | `(year − 1) × 12 + month` |
  | `"YYMMDD"` | days since 1970-01-01 |
  | `"YYMMDDhh"` | days × 24 + hour |
  | `"YYMMDDhhmm"` | days × 1440 + hour × 60 + minute |
  | `"YYMMDDhhmmss"` | days × 86400 + hour × 3600 + minute × 60 + second |

  "days" counts the **complete** days before the date (1970-01-01 → 0), so
  a partial current period never counts as a full unit: on day D the
  hourly period at 14:xx is D × 24 + 14 — the current day contributes its
  completed hours only, never the full 24 h. The month formula counts the
  months of the previous years plus the index of the current month.

  Order, oldest/newest, eviction and resize retention follow the period.
  Physical placement stays with the crash-safe commit protocol — the
  reported bucketId is the logical position.

The caller's record struct contains **only the payload**; the two key
fields are added on write and delivered back on every read through the
`ringFileKeyFields_t` prefix (see the buffer contract below).

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
**bucket (period) order** — the order the timestamps fall in, not the
order of the add calls.

| | Setup A — meter | Setup B — monthly | Setup C — sensor log |
| --- | --- | --- | --- |
| Path | `/storage/meter.rfs` | `/storage/monthly.rfs` | `/storage/sensor.rfs` |
| File type (`schemaId`) | `"YYMMDDhh"` | `"YYMM"` | `"YYMMDDhhmmss"` |
| Payload struct | `meterRecord_t` (8 B) | `monthRecord_t` (12 B) | `sensorRecord_t` (8 B) |
| Stored record | 16 + 8 = 24 B | 16 + 12 = 28 B | 16 + 8 = 24 B |
| Capacity | 3 → resized to 2 | 12 → resized to 24 | 32 → resized to 2 |
| Persistence | immediate | immediate | buffered (`maxPendingRecords` 8) |
| Logical size | 96 + 4×44 = 272 B | 96 + 13×48 = 720 B | 96 + 33×44 = 1548 B |

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

//-- Setup A — hourly meter history, file type "YYMMDDhh".
typedef struct
{
  uint32_t energyWh;       //-- cumulative energy at hour end
  uint16_t errorCount;     //-- errors seen during the hour
  uint16_t pad;            //-- explicit padding, always initialized
} meterRecord_t;           //-- sizeof == 8 (payload only!)

//-- Setup B — monthly totals, file type "YYMM".
typedef struct
{
  uint32_t totalWh;        //-- total energy for the month
  uint16_t outages;        //-- power-loss events in the month
  uint16_t pad;            //-- explicit padding, always initialized
  uint32_t peakW;          //-- peak power in the month
} monthRecord_t;           //-- sizeof == 12 (payload only!)

//-- Setup C — per-second sensor log, file type "YYMMDDhhmmss".
typedef struct
{
  uint32_t tempMilliC;     //-- temperature in milli-degrees C
  uint16_t humidity;       //-- relative humidity in percent
  uint16_t pad;            //-- explicit padding, always initialized
} sensorRecord_t;          //-- sizeof == 8 (payload only!)

#define METER_PATH  "/storage/meter.rfs"
#define MONTH_PATH  "/storage/monthly.rfs"
#define SENS_PATH   "/storage/sensor.rfs"

//-- Read buffers: the component returns the two automatic key fields
//-- first, then the payload. This wrapper receives a full record.
typedef struct
{
  ringFileKeyFields_t keys;   //-- timeStamp + usedKey, added by the component
  meterRecord_t meter;
} fullMeter_t;

typedef struct
{
  ringFileKeyFields_t keys;
  monthRecord_t month;
} fullMonth_t;

typedef struct
{
  ringFileKeyFields_t keys;
  sensorRecord_t sensor;
} fullSensor_t;

//-- Builders: every payload byte (including padding) is initialized
//-- before a record is handed to the component — unchanged-update
//-- detection compares the full stored record, padding included.
static void mkMeter(meterRecord_t *rec, uint32_t energy, uint16_t errors)
{
  memset(rec, 0, sizeof(*rec));
  rec->energyWh = energy;
  rec->errorCount = errors;
}

static void mkMonth(monthRecord_t *rec, uint32_t totalWh, uint16_t outages,
                    uint32_t peakW)
{
  memset(rec, 0, sizeof(*rec));
  rec->totalWh = totalWh;
  rec->outages = outages;
  rec->peakW = peakW;
}

static void mkSensor(sensorRecord_t *rec, uint32_t tempMilliC,
                     uint16_t humidity)
{
  memset(rec, 0, sizeof(*rec));
  rec->tempMilliC = tempMilliC;
  rec->humidity = humidity;
}

static ringFileConfig_t meterConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 3;
  cfg.schemaId = RING_FILE_SCHEMA_YYMMDDHH;   //-- "YYMMDDhh"
  cfg.recordSize = (uint16_t)sizeof(meterRecord_t);
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileConfig_t monthConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 12;
  cfg.schemaId = RING_FILE_SCHEMA_YYMM;       //-- "YYMM"
  cfg.recordSize = (uint16_t)sizeof(monthRecord_t);
  cfg.persistMode = RING_FILE_PERSIST_IMMEDIATE;
  return cfg;
}

static ringFileConfig_t sensorConfig(void)
{
  ringFileConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.maxRecords = 32;
  cfg.schemaId = RING_FILE_SCHEMA_YYMMDDHHMMSS; //-- "YYMMDDhhmmss"
  cfg.recordSize = (uint16_t)sizeof(sensorRecord_t);
  cfg.persistMode = RING_FILE_PERSIST_BUFFERED;
  cfg.maxPendingRecords = 8;
  return cfg;
}

static ringFileOpenConfig_t openCfg(const char *schemaId, uint32_t recordSize,
                                    bool readOnly)
{
  ringFileOpenConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.expectedSchemaId = schemaId;
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

Creates an empty ring **exclusively** and returns it open read-write.
`config->schemaId` selects the file type; `config->recordSize` is the size
of the caller **payload** struct (the two key fields are added by the
component). Fails with `RING_FILE_STORE_ERR_ALREADY_EXISTS` when the path
exists; never recreates or truncates. `ESP_ERR_INVALID_ARG` for a NULL
path/config, an unknown `schemaId` string, `maxRecords == 0`, a
`recordSize` of 0 or an invalid persistence mode / pending limit.

**Example 1 (Setup A):**

```c
ringFileConfig_t cfg = meterConfig();     //-- "YYMMDDhh", capacity 3
ringFileHandle_t *meter = NULL;
ESP_ERROR_CHECK(ringFileCreate(METER_PATH, &cfg, &meter));
```

Expected result: `/storage/meter.rfs` created (272 B logical), handle
open read-write, `count == 0`. A second `ringFileCreate()` on the same
path returns `..._ERR_ALREADY_EXISTS` and leaves the file untouched.

**Example 2 (Setup B):**

```c
ringFileConfig_t cfg = monthConfig();     //-- "YYMM", capacity 12
ringFileHandle_t *monthly = NULL;
ESP_ERROR_CHECK(ringFileCreate(MONTH_PATH, &cfg, &monthly));
```

Expected result: monthly ring created; every record addresses one
calendar month bucket.

**Example 3 (Setup C):**

```c
ringFileConfig_t cfg = sensorConfig();    //-- "YYMMDDhhmmss", buffered
ringFileHandle_t *sensor = NULL;
ESP_ERROR_CHECK(ringFileCreate(SENS_PATH, &cfg, &sensor));

ringFileInfo_t info;
ESP_ERROR_CHECK(ringFileGetInfo(sensor, &info));
//-- info.schemaId == "YYMMDDhhmmss", info.granularity ==
//-- RING_FILE_GRANULARITY_SECOND, info.recordSize == 24 (stored bytes)
```

Expected result: per-second ring created in buffered mode; `info.payloadSize`
is 8, `info.recordSize` 24 (the stored record including the key prefix).

### ringFileOpen

```c
esp_err_t ringFileOpen(const char *path, const ringFileOpenConfig_t *openConfig,
                       ringFileHandle_t **outHandle);
```

Opens an existing ring. The stored `schemaId` string and payload
`recordSize` must equal `expectedSchemaId` / `expectedRecordSize`,
otherwise `RING_FILE_STORE_ERR_SCHEMA_MISMATCH` (format magic/version and
geometry problems report `..._ERR_FORMAT` / `..._ERR_CORRUPT`). A
read-write open takes the exclusive `<path>.lck` lock (a second writable
handle gives `..._ERR_BUSY`) and may perform recovery writes. A read-only
open never writes and fails with `..._ERR_RECOVERY_REQUIRED` when repair
would be needed.

**Example 1 (Setup A):**

```c
ringFileHandle_t *meter = NULL;
ESP_ERROR_CHECK(ringFileOpen(METER_PATH,
                             openCfg(RING_FILE_SCHEMA_YYMMDDHH,
                                     sizeof(meterRecord_t), false),
                             &meter));
```

Expected result: handle open read-write with the persisted capacity and
file type. Passing `RING_FILE_SCHEMA_YYMM` (monthly type) or a different
payload size yields `..._ERR_SCHEMA_MISMATCH`.

**Example 2 (Setup B):**

```c
ringFileHandle_t *monthly = NULL;
ESP_ERROR_CHECK(ringFileOpen(MONTH_PATH,
                             openCfg(RING_FILE_SCHEMA_YYMM,
                                     sizeof(monthRecord_t), false),
                             &monthly));
```

Expected result: monthly ring open read-write.

**Example 3 (Setup C):**

```c
ringFileHandle_t *sensor = NULL;
ringFileOpenConfig_t ro = openCfg(RING_FILE_SCHEMA_YYMMDDHHMMSS,
                                  sizeof(sensorRecord_t), true);
ESP_ERROR_CHECK(ringFileOpen(SENS_PATH, &ro, &sensor));
esp_err_t err = ringFileAdd(sensor, 20261023141501ULL, &somePayload);
//-- err == RING_FILE_STORE_ERR_READONLY
```

Expected result: read-only handle serves reads only; any mutation returns
`..._ERR_READONLY`.

### ringFileClose

```c
void ringFileClose(ringFileHandle_t *handle);
```

Commits staged data (buffered mode), closes the file, releases the lock
and frees the handle. A clean handle performs no write and no sync. On an
I/O failure the storage still exposes a complete previous-or-new state;
the pointer is invalid after the call regardless of the result (the
function has no return value).

**Example 1 (Setup A):**

```c
ringFileClose(meter);
meter = NULL;
```

Expected result: hourly ring flushed (already committed in immediate
mode), lock released.

**Example 2 (Setup B):**

```c
ringFileClose(monthly);
monthly = NULL;
```

Expected result: monthly ring closed; a reopen sees every added bucket.

**Example 3 (Setup C):**

```c
ringFileClose(sensor);
sensor = NULL;
```

Expected result: the buffered close first commits all staged records and
syncs, then closes — after the power-loss-safe flush.

### ringFileAdd

```c
esp_err_t ringFileAdd(ringFileHandle_t *handle, uint64_t timeStamp,
                      const void *record);
```

Appends one record. The component validates the timestamp digits for the
file type, stores the full `timeStamp` as field 1, the truncated
`usedKey` as field 2, and derives the bucketId (`period % capacity`).
Rules:

* Adding a timestamp whose `usedKey` is **already active** updates that
  record in place (no duplicate records, position preserved).
* Otherwise the bucket must be **newer than the newest stored bucket**;
  a clock rewind or gap fill through `add()` returns
  `RING_FILE_STORE_ERR_NOT_NEWER` and changes nothing (use
  `ringFileEdit`).
* At capacity exactly the oldest bucket is evicted.
* Invalid digits for the file type (month 00/13, day 00/31, hour > 23,
  minute/second > 59 — or non-zero sub-granularity digits outside their
  range) are `ESP_ERR_INVALID_ARG`.

**Example 1 (Setup A — hourly, capacity 3):**

```c
meterRecord_t p;
mkMeter(&p, 534668, 0);
ESP_ERROR_CHECK(ringFileAdd(meter, 20261023140000ULL, &p));  //-- hour 14
ESP_ERROR_CHECK(ringFileAdd(meter, 20261023150000ULL, &p));  //-- hour 15
ESP_ERROR_CHECK(ringFileAdd(meter, 20261023160000ULL, &p));  //-- hour 16, full
ESP_ERROR_CHECK(ringFileAdd(meter, 20261023170000ULL, &p));  //-- evicts hour 14

esp_err_t err = ringFileAdd(meter, 20261023100000ULL, &p);
//-- err == RING_FILE_STORE_ERR_NOT_NEWER (hour 10 older than newest 17)
```

Expected result after the four adds: ring `[15, 16, 17]`, each record
storing `timeStamp 20261023150000`… and `usedKey 2026102315`… The refused
add changes nothing. Adding again within an active hour, e.g.
`ringFileAdd(meter, 20261023175959ULL, &p)`, **overwrites** the 17:00
record (same `usedKey 2026102317`) instead of appending.

**Example 2 (Setup B — monthly, capacity 12):**

```c
monthRecord_t p;
mkMonth(&p, 123456789u, 2, 5100u);
//-- Sub-granularity digits are "00" padding for a "YYMM" file:
ESP_ERROR_CHECK(ringFileAdd(monthly, 20260900000000ULL, &p));  //-- Sep 2026
ESP_ERROR_CHECK(ringFileAdd(monthly, 20261000000000ULL, &p));  //-- Oct 2026
```

Expected result: two records; usedKeys `202609` and `202610`; bucketIds
`period % 12` with `period = (2026−1)×12+9 = 24309` and `24310`.

**Example 3 (Setup C — per second, buffered, capacity 32):**

```c
sensorRecord_t p;
mkSensor(&p, 21450, 55);
ESP_ERROR_CHECK(ringFileAdd(sensor, 20261023141500ULL, &p));
ESP_ERROR_CHECK(ringFileAdd(sensor, 20261023141501ULL, &p));
```

Expected result: two distinct buckets (`usedKey` = the full 14 digits,
the file type keeps seconds); staged in RAM (immediately visible to reads
through the same handle until the buffer flushes).

### ringFileReadByKey

```c
esp_err_t ringFileReadByKey(ringFileHandle_t *handle, uint64_t usedKey,
                            void *outRecord);
```

Copies the full stored record — `timeStamp` and `usedKey` first, then the
payload — of the active record with this `usedKey` into the caller's
buffer. The buffer must hold `RING_FILE_FULL_RECORD_SIZE(payloadType)`
bytes. No active record with that key: `..._ERR_KEY_NOT_FOUND`.

**Example 1 (Setup A):**

```c
fullMeter_t out;
ESP_ERROR_CHECK(ringFileReadByKey(meter, 2026102317ULL, &out));
//-- out.keys.timeStamp == 20261023175959 (last write of that hour)
//-- out.keys.usedKey   == 2026102317
//-- out.meter.energyWh == 534668

esp_err_t err = ringFileReadByKey(meter, 2026102314ULL, &out);
//-- err == RING_FILE_STORE_ERR_KEY_NOT_FOUND (evicted)
```

Expected result: the surviving 17:00 record with **both** automatic key
fields filled; the evicted 14:00 bucket reports `KEY_NOT_FOUND`.

**Example 2 (Setup B):**

```c
fullMonth_t out;
ESP_ERROR_CHECK(ringFileReadByKey(monthly, 202610ULL, &out));
//-- out.keys.timeStamp == 20261000000000, out.keys.usedKey == 202610
```

Expected result: October totals; `usedKey` is the 6-digit `EEYYMM` value.

**Example 3 (Setup C):**

```c
fullSensor_t out;
ESP_ERROR_CHECK(ringFileReadByKey(sensor, 20261023141501ULL, &out));
```

Expected result: the staged 14:15:01 sample is served from the RAM buffer
through the same handle.

### ringFileUpdateByKey

```c
esp_err_t ringFileUpdateByKey(ringFileHandle_t *handle, uint64_t usedKey,
                              uint64_t timeStamp, const void *record);
```

Replaces the complete record (payload and stored timestamp) of an active
`usedKey` while preserving its bucket position. The component derives the
`usedKey` from `timeStamp`; a replacement whose derived key differs from
the lookup key is `ESP_ERR_INVALID_ARG`. Replacing with a byte-identical
record (timestamp + payload) performs no write at all. Unknown key:
`..._ERR_KEY_NOT_FOUND`.

**Example 1 (Setup A):**

```c
meterRecord_t p;
mkMeter(&p, 534669, 3);
ESP_ERROR_CHECK(ringFileUpdateByKey(meter, 2026102317ULL,
                                    20261023173000ULL, &p));
//-- stored record now: timeStamp 20261023173000, usedKey 2026102317,
//-- energyWh 534669, errorCount 3 — position unchanged.

esp_err_t err = ringFileUpdateByKey(meter, 2026102317ULL,
                                    20261023183000ULL, &p);
//-- err == ESP_ERR_INVALID_ARG (18:00 derives usedKey 2026102318 ≠ key)
```

Expected result: the 17:00 bucket keeps its position between 16:00 and
the ring end; the mismatched-timeStamp update is rejected without any
change.

**Example 2 (Setup B):**

```c
monthRecord_t p;
mkMonth(&p, 123459999u, 3, 5120u);
ESP_ERROR_CHECK(ringFileUpdateByKey(monthly, 202610ULL,
                                    20261015120000ULL, &p));
```

Expected result: October record revised; the stored `timeStamp` becomes
`20261015120000` (real sub-granularity digits are allowed below the file
type).

**Example 3 (Setup C):**

```c
sensorRecord_t p;
mkSensor(&p, 21999, 56);
esp_err_t err = ringFileUpdateByKey(sensor, 20261023141500ULL,
                                    20261023141500ULL, &p);
```

Expected result: staged revision of 14:15:00 (buffered mode coalesces
repeated revisions of the same bucket).

### ringFileUpsert

```c
esp_err_t ringFileUpsert(ringFileHandle_t *handle, uint64_t timeStamp,
                         const void *record);
```

The update-or-add convenience for a logger revising the *current* bucket:
updates in place when the derived `usedKey` is active, otherwise applies
the `ringFileAdd()` append rules (newer than newest, evict oldest).
Lookup and mutation happen under the handle lock, so the operation is
atomic with respect to other users of this handle.

**Example 1 (Setup A):**

```c
meterRecord_t p;
mkMeter(&p, 535000, 0);
ESP_ERROR_CHECK(ringFileUpsert(meter, 20261023173000ULL, &p));  //-- hour 17 active → update
ESP_ERROR_CHECK(ringFileUpsert(meter, 20261023180000ULL, &p));  //-- hour 18 → append (evicts 15)
```

Expected result: 17:00 revised in place; 18:00 appended as newest, the
oldest (15:00) evicted exactly.

**Example 2 (Setup B):**

```c
monthRecord_t p;
mkMonth(&p, 130000000u, 1, 5200u);
ESP_ERROR_CHECK(ringFileUpsert(monthly, 20261100000000ULL, &p)); //-- new month → append
```

Expected result: November bucket appended (`usedKey 202611`).

**Example 3 (Setup C):**

```c
sensorRecord_t p;
mkSensor(&p, 22100, 57);
for (int i = 0; i < 5; i++)
{
  p.tempMilliC = 22100 + i;
  ESP_ERROR_CHECK(ringFileUpsert(sensor, 20261023141502ULL, &p));
}
//-- five revisions of the same second coalesce into one staged record
```

Expected result: one staged record for 14:15:02 with `tempMilliC 22104`.

### ringFileEdit

```c
esp_err_t ringFileEdit(ringFileHandle_t *handle, uint64_t timeStamp,
                       const void *record);
```

Backfill/edit for measurements that were lost. Updates the bucket when
its `usedKey` is active (any timestamp within the bucket, unlike
`ringFileAdd()`); otherwise inserts the bucket when its period lies
inside the retained window
`[newestPeriod − capacity + 1, newestPeriod]` (an empty ring accepts any
bucket). Outside the window: `RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE`,
nothing changed. At capacity an insert evicts exactly the oldest bucket.

**Example 1 (Setup A):**

```c
meterRecord_t p;
mkMeter(&p, 530000, 0);
esp_err_t err = ringFileEdit(meter, 20261023163000ULL, &p); //-- 16:00 active → update
ESP_ERROR_CHECK(err);
err = ringFileEdit(meter, 20261023130000ULL, &p); //-- evicted long ago
//-- err == RING_FILE_STORE_ERR_BUCKET_OUT_OF_RANGE (window is 16..18)
```

Expected result: the active 16:00 bucket revised with a mid-hour
timestamp; the 13:00 backfill refused (its period is below the window).

**Example 2 (Setup B):**

```c
monthRecord_t p;
mkMonth(&p, 111000000u, 0, 4900u);
ESP_ERROR_CHECK(ringFileEdit(monthly, 20260800000000ULL, &p)); //-- Aug backfill, in window
```

Expected result: August bucket inserted between the retained months
(window of 14 monthly buckets covers well over the added months).

**Example 3 (Setup C):**

```c
sensorRecord_t p;
mkSensor(&p, 20000, 50);
esp_err_t err = ringFileEdit(sensor, 20261023141500ULL, &p); //-- active second → update
```

Expected result: the active 14:15:00 bucket revised.

### ringFileReadOldest / ringFileReadNewest

```c
esp_err_t ringFileReadOldest(ringFileHandle_t *handle, void *outRecord);
esp_err_t ringFileReadNewest(ringFileHandle_t *handle, void *outRecord);
```

Copy the first / last record in **bucket order** (full record, key fields
included). Empty ring: `..._ERR_RING_EMPTY`.

**Example 1 (Setup A):**

```c
fullMeter_t first, last;
ESP_ERROR_CHECK(ringFileReadOldest(meter, &first));  //-- bucket 16
ESP_ERROR_CHECK(ringFileReadNewest(meter, &last));   //-- bucket 18
```

Expected result: `first.keys.usedKey == 2026102316`,
`last.keys.usedKey == 2026102318`.

**Example 2 (Setup B):**

```c
fullMonth_t last;
ESP_ERROR_CHECK(ringFileReadNewest(monthly, &last));
//-- last.keys.usedKey == 202611 (newest month)
```

**Example 3 (Setup C):**

```c
fullSensor_t last;
ESP_ERROR_CHECK(ringFileReadNewest(sensor, &last));
//-- newest staged second == 20261023141502
```

### ringFileIteratorBegin / Next / Previous / GetSlot

```c
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
esp_err_t ringFileIteratorGetSlot(ringFileHandle_t *handle,
                                  const ringFileIterator_t *iterator,
                                  uint32_t *outSlot);
```

`Begin` positions the cursor and copies the starting record (full record
with key fields). `RING_FILE_ITER_KEY` requires a non-NULL `usedKey` and
starts at that record (`..._ERR_KEY_NOT_FOUND` when inactive). `Next` /
`Previous` walk in bucket order and stop with `..._ERR_ITER_END` at the
logical ends. Each concurrent traversal gets its own cursor. A successful
mutation on the handle invalidates existing cursors
(`..._ERR_ITER_INVALIDATED`) — call `Begin` again. `GetSlot` reports the
**bucketId** (`period % capacity`) of the record the cursor last returned
— the logical slot; a not-yet-flushed staged record
reports `RING_FILE_SLOT_STAGED`.

**Example 1 (Setup A):**

```c
ringFileIterator_t it;
fullMeter_t rec;
memset(&it, 0, sizeof(it));
ESP_ERROR_CHECK(ringFileIteratorBegin(meter, &it, RING_FILE_ITER_OLDEST,
                                      NULL, &rec));
for (;;)
{
  uint32_t bucket;
  ESP_ERROR_CHECK(ringFileIteratorGetSlot(meter, &it, &bucket));
  printf("bucket=%lu usedKey=%llu energy=%lu\n", (unsigned long)bucket,
         (unsigned long long)rec.keys.usedKey,
         (unsigned long)rec.meter.energyWh);

  if (ringFileIteratorNext(meter, &it, &rec) != ESP_OK)
  {
    break;
  }
}
```

Expected result: the buckets `16, 17, 18` (in period order) with their
bucketIds (`period % capacity`) and payloads. After any successful
mutation on `meter` the next `Next()` returns `..._ERR_ITER_INVALIDATED`.

**Example 2 (Setup B):**

```c
uint64_t key = 202610;
ringFileIterator_t it;
fullMonth_t rec;
memset(&it, 0, sizeof(it));
ESP_ERROR_CHECK(ringFileIteratorBegin(monthly, &it, RING_FILE_ITER_KEY,
                                      &key, &rec));
while (ringFileIteratorPrevious(monthly, &it, &rec) == ESP_OK)
{
  //-- walk backwards: September, August, ...
}
```

Expected result: starts at the October bucket and walks to older months.

**Example 3 (Setup C):**

```c
ringFileIterator_t it;
fullSensor_t rec;
memset(&it, 0, sizeof(it));
ESP_ERROR_CHECK(ringFileIteratorBegin(sensor, &it, RING_FILE_ITER_NEWEST,
                                      NULL, &rec));
uint32_t bucket;
ESP_ERROR_CHECK(ringFileIteratorGetSlot(sensor, &it, &bucket));
//-- newest is staged → bucket == RING_FILE_SLOT_STAGED until synced
```

Expected result: newest staged second reported as `RING_FILE_SLOT_STAGED`
before `ringFileSync()`; after the sync `GetSlot` reports its bucketId.

### ringFileResize

```c
esp_err_t ringFileResize(ringFileHandle_t *handle, uint32_t newCapacity);
```

Changes capacity, preserving the relative bucket order; on shrink the
**newest buckets that fit** are retained. `newCapacity >= 1`; capacity 1
is supported. Runs through a verified candidate file on the same volume
(temporary space ≈ header + (retained + 1) × slotSize); pending staged
data is committed first; the committed ring is preserved on recoverable
errors. Returns to a usable handle even on candidate-build failure
(`..._ERR_STORAGE_FULL` when the volume is full).

**Example 1 (Setup A):**

```c
ESP_ERROR_CHECK(ringFileResize(meter, 2));
//-- ring [16, 17, 18] → [17, 18]: oldest bucket 16 dropped
```

Expected result: capacity 2 persisted; logical size 96 + 3×44 = 228 B.

**Example 2 (Setup B):**

```c
ESP_ERROR_CHECK(ringFileResize(monthly, 24));
//-- capacity 24; existing month buckets keep their relative order
```

Expected result: two more years of monthly history fit.

**Example 3 (Setup C):**

```c
ESP_ERROR_CHECK(ringFileResize(sensor, 2));
//-- newest two seconds retained, older staged/committed records evicted
```

Expected result: `count == 2`, newest buckets retained.

### ringFileGetInfo

```c
esp_err_t ringFileGetInfo(ringFileHandle_t *handle, ringFileInfo_t *outInfo);
```

Snapshot: `count`, `capacity`, `payloadSize`, `recordSize` (stored bytes
= `payloadSize + 16`), `schemaId[13]`, `granularity`, `storageSize`,
`pendingCount`, `dirty`.

**Example 1 (Setup A):**

```c
ringFileInfo_t info;
ESP_ERROR_CHECK(ringFileGetInfo(meter, &info));
//-- strcmp(info.schemaId, "YYMMDDhh") == 0
//-- info.granularity == RING_FILE_GRANULARITY_HOUR
//-- info.capacity == 2, info.recordSize == 24
```

**Example 2 (Setup B):**

```c
ESP_ERROR_CHECK(ringFileGetInfo(monthly, &info));
//-- strcmp(info.schemaId, "YYMM") == 0, granularity == ..._MONTH
```

**Example 3 (Setup C):**

```c
ESP_ERROR_CHECK(ringFileGetInfo(sensor, &info));
//-- granularity == ..._SECOND; pendingCount > 0 and dirty == true while
//-- buffered revisions are outstanding
```

### ringFileSync

```c
esp_err_t ringFileSync(ringFileHandle_t *handle);
```

Commits pending buffered changes and synchronizes through the backend. A
clean handle performs no write and no sync. Read-only handles with nothing
staged return `ESP_OK`, with staged data `..._ERR_READONLY` (cannot
happen); after an ambiguous I/O failure `..._ERR_IO_FAULTED`.

**Example 1 (Setup C):**

```c
ESP_ERROR_CHECK(ringFileSync(sensor));
ringFileInfo_t info;
ESP_ERROR_CHECK(ringFileGetInfo(sensor, &info));
//-- info.pendingCount == 0, info.dirty == false
```

Expected result: staged per-second records now on storage; `GetSlot`
reports real bucketIds instead of `RING_FILE_SLOT_STAGED`.

### ringFileValidate

```c
esp_err_t ringFileValidate(const char *path, ringFileValidation_t *outValidation);
```

Stand-alone read-only integrity check; never opens a handle and never
writes. Reports `headerValid`, `metadataIssue` (magic / format version /
checksum / geometry / short file), `capacity`, `recordSize`, `slotCount`,
`badSlotCount`, `committedCount` and `strayResizeFiles`.

**Example 1 (Setup A):**

```c
ringFileValidation_t v;
ESP_ERROR_CHECK(ringFileValidate(METER_PATH, &v));
//-- v.headerValid == true, v.badSlotCount == 0, v.strayResizeFiles == false
```

**Example 2 (Setup B):**

```c
ringFileValidation_t v;
ESP_ERROR_CHECK(ringFileValidate(MONTH_PATH, &v));
//-- v.committedCount == number of retained month buckets
```

**Example 3 (missing file):**

```c
ringFileValidation_t v;
esp_err_t err = ringFileValidate("/storage/nope.rfs", &v);
//-- err == ESP_ERR_NOT_FOUND
```

### ringFileClear

```c
esp_err_t ringFileClear(ringFileHandle_t *handle);
```

Explicitly discards all records; the file type and capacity are retained.
Staged data is dropped and the header epoch is advanced so earlier
records stay hidden. Uses the same commit rules as the other header
writes (create/resize).

**Example 1 (Setup A):**

```c
ESP_ERROR_CHECK(ringFileClear(meter));
//-- count == 0; capacity and "YYMMDDhh" type unchanged; old slots stay
//-- on storage but are hidden by the epoch.
```

**Example 2 (Setup B):**

```c
ESP_ERROR_CHECK(ringFileClear(monthly));
//-- next add starts a fresh month history; period ordering rules apply
//-- again from the empty state (an empty ring accepts any bucket).
```

**Example 3 (Setup C):**

```c
ESP_ERROR_CHECK(ringFileClear(sensor));
//-- pending staged samples are dropped (never committed)
```

### ringFileBreakStaleLock

```c
esp_err_t ringFileBreakStaleLock(const char *path);
```

Removes a lock file `<path>.lck` left behind by a run that died before
`ringFileClose()` (panic, reset, power loss). Call only right after a
device reset, before the first open — calling it while a live handle
exists breaks that handle's protection. `ESP_OK` when a lock was removed,
`ESP_ERR_NOT_FOUND` when none existed. Never touches the ring file itself.

**Example 1 (Setup A):**

```c
if (ringFileBreakStaleLock(METER_PATH) == ESP_OK)
{
  ESP_LOGW(TAG, "cleared stale lock left by an interrupted run");
}
ESP_ERROR_CHECK(ringFileOpen(METER_PATH, openCfg(RING_FILE_SCHEMA_YYMMDDHH,
                                                 sizeof(meterRecord_t),
                                                 false),
                             &meter));
```

Expected result: a reopen that would have failed with `..._ERR_BUSY`
after a hard reset now succeeds.

### Timestamp helpers

```c
esp_err_t ringFileGranularityFromSchemaId(const char *schemaId,
                                          ringFileGranularity_t *outGranularity);
esp_err_t ringFileTimeStampToUsedKey(uint64_t timeStamp,
                                     ringFileGranularity_t granularity,
                                     uint64_t *outUsedKey);
esp_err_t ringFileTimeStampToPeriod(uint64_t timeStamp,
                                    ringFileGranularity_t granularity,
                                    uint64_t *outPeriod);
esp_err_t ringFileTimeStampStringToUnix(const char *timeStamp,
                                        uint64_t *outUnixSeconds);
esp_err_t ringFileUnixToTimeStampString(uint64_t unixSeconds,
                                        char outTimeStamp[15]);
esp_err_t ringFileUnixToBucketId(uint64_t unixSeconds,
                                 ringFileGranularity_t granularity,
                                 uint64_t *outBucketId);
esp_err_t ringFileUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                                uint32_t *outIsoWeek);
```

Usable with or without an open ring. The `…ToUsedKey` / `…ToPeriod`
helpers validate the 14-digit `EEYYMMDDhhmmss` value for the granularity
(century `20`; month 01–12, day valid for the month including leap years,
hour 00–23, minute/second 00–59; fields **below** the granularity may be
`00` padding or a valid value) and return `ESP_ERR_INVALID_ARG`
otherwise. `ringFileUnixToBucketId()` computes the period from unix
seconds; `ringFileUnixToIsoWeek()` returns the ISO-8601 week-numbering
year/week for display (weeks start Monday; 2025-12-29 is ISO 2026-W01).
The string↔unix pair converts between unix seconds and the 14-digit
string (`"20000101000000"` ↔ `946684800`).

```c
ringFileGranularity_t g;
ESP_ERROR_CHECK(ringFileGranularityFromSchemaId(RING_FILE_SCHEMA_YYMM, &g));
//-- g == RING_FILE_GRANULARITY_MONTH

uint64_t usedKey, period;
ESP_ERROR_CHECK(ringFileTimeStampToUsedKey(20261023143055ULL, g, &usedKey));
//-- usedKey == 202610
ESP_ERROR_CHECK(ringFileTimeStampToPeriod(20261023143055ULL, g, &period));
//-- period == (2026 - 1) * 12 + 10 == 24310 (month formula)

uint32_t slot = (uint32_t)(period % 12);   //-- bucketId for capacity 12 → 10
```

### Practical update-or-add pattern (hourly logger)

Accumulate the current hour in RAM and commit at the hourly boundary:

```c
meterRecord_t p;
mkMeter(&p, energyWh, errorCount);
ESP_ERROR_CHECK(ringFileUpsert(meter, 20261023140000ULL, &p));
```

In immediate mode that call is one committed, synchronized operation. In
buffered mode the same pattern stages in RAM; call `ringFileSync()` at the
hourly boundary (and at any interval below which lost data is
unacceptable) — the power-loss window is everything staged since the last
sync/close. See `main/main.c` for the complete example.

### Result codes (quick reference)

Standard ESP-IDF codes: `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`,
`ESP_ERR_INVALID_STATE`, `ESP_ERR_NOT_FOUND`, `ESP_ERR_INVALID_SIZE`.
Custom codes share base `RING_FILE_STORE_ERR_BASE` (0x5600):

| Code | Cause |
| --- | --- |
| `..._ERR_RING_EMPTY` | readOldest/readNewest/iterator begin on a ring with no records |
| `..._ERR_KEY_NOT_FOUND` | no active record carries the `usedKey` |
| `..._ERR_DUPLICATE_KEY` | reserved — no longer produced (adding an active bucket updates it) |
| `..._ERR_ITER_END` | iterator reached the logical end in that direction |
| `..._ERR_ITER_INVALIDATED` | a successful mutation invalidated the cursor |
| `..._ERR_SCHEMA_MISMATCH` | stored `schemaId`/payload size ≠ expectation |
| `..._ERR_FORMAT` | magic, format version or geometry not understood |
| `..._ERR_CORRUPT` | integrity failure; recovery cannot determine a valid state |
| `..._ERR_READONLY` | mutation attempted through a read-only handle |
| `..._ERR_IO` | backend I/O failure (short write, fsync, rename) |
| `..._ERR_RECOVERY_REQUIRED` | read-only open found a state that needs writing to recover |
| `..._ERR_IO_FAULTED` | handle quarantined after an ambiguous I/O failure; close and reopen |
| `..._ERR_STORAGE_FULL` | resize candidate could not be created/filled (volume full) |
| `..._ERR_ALREADY_EXISTS` | create refused to overwrite an existing file |
| `..._ERR_BUSY` | another writable handle holds `<path>.lck` |
| `..._ERR_NOT_NEWER` | `ringFileAdd`/`ringFileUpsert` add path for a bucket not newer than the newest stored bucket (use `ringFileEdit`) |
| `..._ERR_BUCKET_OUT_OF_RANGE` | `ringFileEdit` for a bucket outside the retained window |

`esp_err_to_name()` prints a bare `"ERROR"` for these custom codes — the
example maps them with a local `rfsErrName()` helper (see `main/main.c`).

### Buffer contract (recap)

* **Payload** buffers (`ringFileAdd`, `ringFileUpsert`, `ringFileEdit`,
  `ringFileUpdateByKey`) are caller-owned and at least `recordSize`
  bytes — the payload struct only; the component adds the two key fields.
* **Output record** buffers (`ringFileReadByKey`, `ringFileReadOldest`,
  `ringFileReadNewest`, `ringFileIteratorBegin/Next/Previous`) must hold
  `RING_FILE_FULL_RECORD_SIZE(payloadType)` bytes: the
  `ringFileKeyFields_t` prefix (`timeStamp`, then `usedKey`) followed by
  the payload. The component fills all of it.
* `memset` the whole payload struct before filling it — unchanged-update
  detection compares the full stored record, padding included.
* Timestamp arguments are numeric `EEYYMMDDhhmmss` `uint64_t` values
  (14 digits, century `20`); digits below the file type are validated as
  zero padding or valid values but never key-relevant.
* Key arguments are plain `uint64_t usedKey` values (the truncated
  timestamp digits), e.g. `2026102317` for hour 14 on 23-10-2026 in a
  `"YYMMDDhh"` file.
* Payloads must be pointer-free with fixed-width fields and are stored as
  raw bytes: reopening requires the same payload layout, ABI and
  little-endian byte order — any layout change needs a new file.
* Integers are stored little-endian and read without unaligned loads.
