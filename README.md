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
buffered-mode staging and a read-only handle.

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
takeover after an interrupted run.

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
