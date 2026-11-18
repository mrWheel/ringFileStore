# Project log

Format: DD-MM-YYYY - HH:MM — what was tried, result, errors/findings.
See the repository README for the logging rules. All documentation in
English.

## 18-11-2026

- 14:05 — Started implementation from `projectPrompt.md` (scaffold state:
  build probe only, previous build had failed with target `esp32` vs the
  component's `esp32s3` restriction). Verified ESP-IDF 6.0.2 at
  `~/.espressif/v6.0.2/esp-idf`; fatfs, sdmmc, esp_driver_sdmmc, vfs all
  present in the IDF tree; LittleFS not in core → managed component
  `joltwallet/littlefs`.
- 14:20 — Designed the binary format: 64-byte explicitly encoded header +
  (capacity + 1) fixed slots of [20-byte slot header | record]; committed
  state derived from per-slot (seq, rev, CRC32); mutations commit exactly
  one slot image and never overwrite a committed slot in place; header only
  written by create, resize and clear. Wrote public header (API, error enum,
  buffer contract) and component source.
- 14:40 — Wrote host test harness: POSIX shims (esp_err/esp_log/FreeRTOS
  mutex) + failure-injection table over the internal file ops. First
  compile fixes: missing FreeRTOS include in shim.c, uninitialized record
  in a test.
- 14:55 — First test run: core CRUD/keys/iterator/wraparound passed.
  Real component bugs found and fixed:
  * `ringFileClear` epoch change was never written to the header on flush
    (records reappeared after reopen) — flush now commits the header.
  * `ringFileIteratorBegin(OLDEST)` missed seq 0 (exclusive cursor
    comparison) — added inclusive forward comparison.
  * `ringFileResize` closed the original fd before streaming membership
    reads → candidate selection always failed (INVALID_STATE); the original
    now stays open until the verified switch.
  * Test-injection bug (not component): multiple `failNextOn` patterns
    shared one rule slot; widened to four slots.
- 15:10 — Test-expectation corrections (key 38 stays active after wraps;
  eviction of key 10 at capacity 4; BUSY test needed a live lock holder;
  stats snapshot must be taken after close so the close flush counts;
  oversized-file test now appends past the nominal maximum).
- 15:20 — Host suite green: **565 checks, 0 failures**.
- 15:30 — Example app: LittleFS + SD/FATFS backends behind Kconfig, hourly
  update-or-add logger (immediate and buffered variants), resize and
  validate demos, read-only handle, partitions.csv (storage littlefs
  partition), sdkconfig.defaults updates.
- 15:35 — `idf.py set-target esp32s3` failed: component name is
  `littlefs`, not `esp_littlefs`, in main REQUIRES → fixed. `joltwallet/
  littlefs 1.23.1` resolved from the registry.
- 15:40 — First `idf.py build` failed in main.c: `esp_vfs_littlefs_conf_t`
  field is `base_path` (not `base_prefix`) and three `&openConfig(...)`
  calls took the address of a temporary → fixed with local variables.
- 15:45 — `idf.py build` succeeded: bootloader 0x5240, app
  ringFileStore.bin 0x361a0 (~217 KB, 87% free in the 1.7 MB factory
  partition). No warnings from component or main sources.
- Notes for later: hardware flashing/testing is manual (not performed
  here); target-side Unity app not added; LittleFS block program/erase
  counters not instrumented (host measurements are logical VFS I/O);
  LICENSE decision still open per PUBLISHING.md.

- 16:10 — Developer will test on an ESP32-WROVER (classic ESP32, 8 MB
  PSRAM). The component was restricted to `esp32s3` in `idf_component.yml`
  → added `esp32`. Added target-specific SD pin defaults in
  `main/Kconfig.projbuild` (classic ESP32 SDMMC defaults CLK 14, CMD 15,
  D0 2, D1 4, D2 12, D3 13) and `sdkconfig.defaults.esp32`
  (`CONFIG_SPIRAM=y`). Rebuilding the example for target `esp32`.
- 16:35 — First hardware run on the WROVER: the board flashed and booted
  (PSRAM 8 MB detected, LittleFS mounted), but the second boot aborted in
  `app_main` with `0x560f` = `RING_FILE_STORE_ERR_BUSY` at the reopen and
  reboot-looped: a previous run had died before `ringFileClose()`, leaving
  `/rfs/meter.rfs.lck` behind, which no later boot could release. Fix: new
  component API `ringFileBreakStaleLock(path)` removes a leftover lock
  file (documented for use right after a reset, before the first open);
  the example calls it at startup. Added host test `staleLockBreak`
  (BUSY while held → break → NOT_FOUND → reopen sees the data). Host
  suite: **573 checks, 0 failures**. `idf.py build` (esp32) OK, app
  0x3a5f0, 86% free. Cause of the *original* interrupted run still
  unknown (no log of that boot); next flash will show where it stops.
- 15:11 — S3 board (DevKitC-1-N32R8V, WROOM-2): boot abort
  `quad_psram: PSRAM chip is not connected` / `Failed to init external
  RAM`. Cause: the config assumed quad PSRAM + 4 MB flash; the module is
  octal (OPI) 32 MB flash + 8 MB octal PSRAM at 1.8 V. Set
  `sdkconfig.defaults.esp32s3` + `sdkconfig` to `ESPTOOLPY_OCT_FLASH`,
  `FLASHMODE_OPI`, `FLASH_MODE_AUTO_DETECT=n`, `FLASHSIZE_32MB`,
  `SPIRAM_MODE_OCT`, `SPIRAM_SPEED_80M`. Boot now correct: octal PSRAM
  8 MB + mxic OPI flash, memtest OK. Two stumbling blocks on the way:
  Finder copy `managed_components/joltwallet__littlefs 2` moved away
  (CMake refused it), component re-fetched. First run after that: task
  watchdog abort (IDLE0/CPU0) in the first
  `ringFileClose -> rfsReleaseLock -> unlink` (LittleFS, backtrace anchor
  `cache_utils.c:173` = waiting for CPU1 IPC before cache-disable). ROUND
  1: yields (`vTaskDelay`) between the flash-heavy demo phases +
  `ESP_TASK_WDT_TIMEOUT_S=10`. Identical trace → not resolved; the
  stage-1 hardware reset also explains the repeatedly left-behind stale
  lock file.
- 15:35 — Instrumentation + diagnostic pass: (a) the test program now
  shows the full ring contents from oldest to newest after EVERY step
  (`dumpRing()`, new->old via iterator; called after create/open, every
  add/update, hourly commit, buffered staged/sync, resize 4->6, add 900,
  shrink->3, reopen, read-only); (b)
  `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=n` (sdkconfig +
  defaults.esp32s3) so the idle-task watchdog checks no longer trigger
  the hardware reset loop; the watchdog itself stays on (stage-0
  backtrace). Build and flash/test on /dev/tty.usbserial-1430 by Claude
  (authorized by the user for this project/board).
- 16:40 — Full ring dumps after every test-program step active
  (`dumpRing()`); main.c FileVersion [5]. TWDT idle-checks off
  (CPU0/CPU1) to break the reset loop; the main task itself is subscribed
  to the TWDT and fed after every step, so a hang produces a real
  backtrace of the stuck task. Build/flash/erase by Claude on
  /dev/tty.usbserial-1430 (authorized).
  Results (serial captures):
  * The run now gets past the first `ringFileClose` (close/reopen
  succeed) and then hangs somewhere in a later LittleFS commit; the
  position shifts per boot → race, no fixed deadlock location.
  * Backtrace (main task, CPU0): spin in
  `spi_flash_disable_interrupts_caches_and_other_cpu()`
  (cache_utils.c:175, waiting for `s_flash_op_can_start`) reached via
  `ringFileClose -> rfsReleaseLock -> unlink -> lfs_stat -> lfs_dir_fetch
  -> esp_flash_read -> spi1_start`; CPU1 shows IDLE1 and the ipc1 task
  (esp_ipc_call_nonblocking) never acked. The earlier "844 ms" anomaly is
  explained: the esp_timer ISR is not IRAM-safe, so log timestamps freeze
  at the moment of the hang; the TWDT ISR (which is IRAM) keeps printing
  with the frozen time.
  * Experiment 1: `CONFIG_SPIRAM=n` → same hang at the same place. The
  PSRAM/MSPI-sharing hypothesis is dropped.
  * Experiment 2: `CONFIG_FREERTOS_UNICORE=y` → no hang but a crash:
  `LoadProhibited` in `heap_caps_free()` <- `free()` <-
  `esp_littlefs_free_fd()` (esp_littlefs.c:1751, `free(file)`) <-
  `vfs_littlefs_close` <- `close()` <- `rfsIoClose` in `ringFileClose`
  (first close). EXCVADDR=0x2c. => Root cause is HEAP CORRUPTION (the
  CPU1/IPC hang was a symptom: the corruption hit the ipc1 control
  block). Next step: `CONFIG_HEAP_POISONING_COMPREHENSIVE=y` +
  `heap_caps_check_integrity_all(true)` after every demo step; PSRAM
  stays off for this round (scanning the 8 MB PSRAM heap takes longer
  than the interrupt WDT allows).
- 16:40 (continued) — Heap-debug round: `CONFIG_HEAP_POISONING_COMPREHENSIVE`
  + integrity check after every step (PSRAM briefly off, because scanning
  the 8 MB PSRAM heap exceeded the interrupt WDT). Crash locations kept
  moving (littlefs fd table, VFS table during ESP_LOGI) and the heap
  check had still passed just before the crash → wild writer, not a
  classic heap leak. Suspect: the main task's 3584-byte stack with the
  deep call chain (demo → component → littlefs → ESP_LOG) and the new
  per-step ring dumps.
  TEST: `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` + stack end watchpoint →
  run7: the demo ran completely to "example finished", all ring dumps
  correct, validate header=1 badSlots=0 strays=0, one boot, no resets.
  => ROOT CAUSE: main-task stack overflow; the earlier "IPC/CPU1 hang"
  and the random crashes were symptoms of heap corruption caused by the
  overrunning stack. Host suite (573 checks) under ASan/UBSan: 0 errors
  (the component itself is memory-clean).
  Restored production configuration: PSRAM back on, dual-core back on,
  heap poisoning and stack watchpoint back off, idle-task WDT checks back
  on; main stack stays 8192 (the fix), recorded in
  `sdkconfig.defaults.esp32s3`. Next: erase-flash + clean acceptance run
  (create path incl. demoRingOperations).
- 17:14 — Acceptance test passed. Production configuration restored
  (dual-core on, octal PSRAM on, heap poisoning and stack watchpoint off,
  idle-task WDT checks on, main stack 8192 as the permanent fix in
  `sdkconfig.defaults.esp32s3`). After `erase-flash` (clean LittleFS) one
  boot: create path fully executed (demoRingOperations with correct
  evict: `add 400` evicts oldest 100; update 300 → errors=7 preserves
  position), then hourly logger, buffered staged/sync, resize 4->6, add
  900, shrink->3, validate (header=1 committed=3 badSlots=0 strays=0),
  read-only open and unmount; last line "example finished", no watchdog,
  no crashes. Then corrected the labels of two ring dumps (the eviction
  belongs to add 400, not add 50) and rebuilt/flashed: that build also
  runs completely. Host suite stays 573 checks / 0 failures (also under
  ASan/UBSan). Component README: section "Filesystem, locking and task
  context" extended with the requirement of a >= 8 KB task stack for
  calling tasks.
- 19:05 — Feature round per developer feedback (all documentation in
  English):
  * **Key position mask**: `ringFileKeyDescriptor_t.keyPositionMask`
    (uint64_t) selects the significant byte positions of the key; 0 = all
    significant. Applied in every key comparison (duplicate check, keyed
    read, update key match, upsert, iterator-by-key); persisted in the
    file header. Header grew 64 → 96 bytes (keyPositionMask at 56, CRC
    over [0,88)), format version 1 → 2; old files are rejected explicitly
    (`BAD_FORMAT_VERSION`/`SHORT_FILE`), so the board was erased before
    the acceptance run. Mask validation + persistence + string/mask
    semantics documented in the public header and README.
  * **Slot reporting**: `ringFileIteratorGetSlot()` returns the physical
    slot of the last returned record (`RING_FILE_SLOT_STAGED` for a
    staged record); `ringFileInfo_t.keyPositionMask` added. Example
    `dumpRing()` lines now show `key=` and `slot=` per record;
    `logRecord()` prints the key explicitly.
  * **More records**: example ring capacity 4 → 10 (12 adds: fills the
    ring, evicts 100 and 300; update of 400 preserves position) and new
    host test `moreRecords` (capacity 10, 12 adds, key+slot dumps).
  * **Key mask demo**: example `demoKeyMaskRing()` (second ring
    `keymask.rfs`, mask 0xFC: byte positions 0-1 of the timestamp key
    insignificant) + host test `keyPositionMask` (duplicate-by-mask,
    lookup, update key match, mask survives reopen, invalid masks
    rejected).
  * **testLog.md**: the host suite now writes `testLog.md` (repository
    root, path passed by `run_tests.sh`) recording every logged command
    with its data and its result, ring dumps with key and slot per
    record, a per-test overview table and the PASS/FAIL summary.
  * Host suite: **633 checks, 0 failures**. `idf.py build` (esp32s3) OK,
    app 0x394c0, 87% free.
- 19:40 — Developer instruction: ALL documentation in English (this file
  translated from Dutch; testLog.md generator rewritten in English).
  Findings and fixes from the first hardware run:
  * `esp_err_to_name()` prints a bare "ERROR" for every custom
    `RING_FILE_STORE_ERR_*` code (IDF's generated lookup table does not
    contain them). Added a local `rfsErrName()` helper in the example and
    a full custom-code name table in the test shim, so serial output and
    testLog.md show `DUPLICATE_KEY`, `KEY_NOT_FOUND`, etc.
  * Key-mask demo bug (example, not component): the second add used
    `base + 512`, which changes only byte 1 — an insignificant position —
    so the add correctly returned DUPLICATE_KEY instead of appending.
    Changed to `base + 65536` (byte 2 differs). The demo now calls
    `ringFileClear()` at the start so every boot logs the identical
    scenario.
  * First clean-boot acceptance run (erase-flash, then esptool
    `write-flash @flash_args --after no-reset` + RTS reset under a pyserial
    capture; note: `erase-flash` erases the whole flash, so a full re-flash
    is required afterwards): single boot to "example finished". Created
    ring at capacity 10; add 650 fills it; add 700 evicts oldest 100; add
    750 evicts oldest 300; `read 100 (evicted): KEY_NOT_FOUND`; update 400
    (errors=7) keeps its position; hourly logger and buffered demo show
    `key=` + `slot=` per dump line (staged record prints `slot=staged`);
    validate header=1 committed=3 badSlots=0 strays=0; key-mask ring logs
    `add key=1765000047 (only bytes 0-1 differ): DUPLICATE_KEY` and
    `add key=1765065536 (byte 2 differs): ESP_OK`, dump lists key and
    slot 0/1. Host suite stays 633 checks / 0 failures; testLog.md written
    in English with named results, key/slot dumps and the summary table.
