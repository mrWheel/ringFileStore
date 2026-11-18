# Project log

Format: DD-MM-YYYY - HH:MM — what was tried, result, errors/findings.
See the repository README for the logging rules.

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
  written by create/resize/clear. Wrote public header (API, error enum,
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
- 15:11 — S3 bord (DevKitC-1-N32R8V, WROOM-2): boot abort
  `quad_psram: PSRAM chip is not connected` / `Failed to init external RAM`.
  Oorzaak: config ging uit van quad PSRAM + 4 MB flash; module is octal
  (OPI) flash 32 MB + octal PSRAM 8 MB op 1.8 V. `sdkconfig.defaults.esp32s3`
  + `sdkconfig` gezet op `ESPTOOLPY_OCT_FLASH`, `FLASHMODE_OPI`,
  `FLASH_MODE_AUTO_DETECT=n`, `FLASHSIZE_32MB`, `SPIRAM_MODE_OCT`,
  `SPIRAM_SPEED_80M`. Boot nu correct: octal PSRAM 8 MB + mxic OPI flash,
  memtest OK. Twee struikelblokken onderweg: Finder-kopie
  `managed_components/joltwallet__littlefs 2` weggezet (CMake weigerde),
  component opnieuw opgehaald. Eerste run daarna: taak-watchdog abort
  (IDLE0/CPU0) in de eerste `ringFileClose -> rfsReleaseLock -> unlink`
  (LittleFS, backtraceanker `cache_utils.c:173` = wachten op CPU1 IPC vóór
  cache-disable). RONDE 1: yields (`vTaskDelay`) tussen de flash-zware
  demo-fases + `ESP_TASK_WDT_TIMEOUT_S=10`. Identieke trace -> niet opgelost;
  de stage-1 hardware reset verklaart ook het telkens achterblijven van het
  stale lock-bestand.
- 15:35 — Instrumentatie + diagnose-pas: (a) testprogramma toont na IEDERE
  stap de volledige ringinhoud van oud naar nieuw (`dumpRing()`, new->old via
  iterator; aanroepen na create/open, elke add/update, hourly commit,
  buffered staged/sync, resize 4->6, add 900, shrink->3, reopen, read-only);
  (b) `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=n` (sdkconfig +
  defaults.esp32s3) zodat de idle-task watchdog-checks de hardware-reset-lus
  niet meer triggers; watchdog zelf blijft aan (stage0 backtrace). Build en
  flash/test op /dev/tty.usbserial-1430 door Claude (door gebruiker
  geautoriseerd voor dit project/bord).
- 16:40 — Volledige ringdumps na elke stap in het testprogramma actief
  (`dumpRing()`); main.c FileVersion [5]. TWDT idle-checks uit
  (CPU0/CPU1) om de reset-lus te doorbreken; main-task zelf subscribed
  aan de TWDT en wordt na elke stap gefeed, zodat een hang een echte
  backtrace van de vastlopende taak geeft. Build/flash/erase door Claude
  op /dev/tty.usbserial-1430 (geautoriseerd).
  Resultaten (seriële captures):
  * De run komt nu verder dan de eerste `ringFileClose` (close/reopen
  lukken) en blijft daarna ergens in een LittleFS-commit hangen; positie
  verschuift per boot -> race, geen vaste deadlockplek.
  * Backtrace (main-task, CPU0): spin in
  `spi_flash_disable_interrupts_caches_and_other_cpu()`
  (cache_utils.c:175, wachten op `s_flash_op_can_start`) bereikt via
  `ringFileClose -> rfsReleaseLock -> unlink -> lfs_stat -> lfs_dir_fetch
  -> esp_flash_read -> spi1_start`; CPU1 toont IDLE1 en de ipc1-taak
  (esp_ipc_call_nonblocking) acked nooit. De eerdere "844 ms"-anomalie is
  verklaard: de esp_timer-ISR is niet IRAM-safe, dus de log-timestamps
  bevriezen op het moment van de hang; de TWDT-ISR (wel IRAM) blijft
  afdrukken met de bevroren tijd.
  * Experiment 1: `CONFIG_SPIRAM=n` -> zelfde hang op dezelfde plaats.
  PSRAM/MSPI-delingshypothese vervallen.
  * Experiment 2: `CONFIG_FREERTOS_UNICORE=y` -> geen hang maar een
  crash: `LoadProhibited` in `heap_caps_free()` <- `free()` <-
  `esp_littlefs_free_fd()` (esp_littlefs.c:1751, `free(file)`) <-
  `vfs_littlefs_close` <- `close()` <- `rfsIoClose` in `ringFileClose`
  (eerste close). EXCVADDR=0x2c. => onderliggende oorzaak is
  HEAPCORRUPTIE (de CPU1/IPC-hang was een symptoom: corruptie raakte de
  ipc1-besturing). Volgende stap: `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`
  + `heap_caps_check_integrity_all(true)` na elke demo-stap; PSRAM blijft
  uit voor deze ronde (scan van de 8 MB PSRAM-heap duurt langer dan de
  interrupt-WDT-toestond).
- 16:40 (vervolg) — Heap-debugronde: `CONFIG_HEAP_POISONING_COMPREHENSIVE`
  + integrity-check na elke stap (PSRAM kort uit, want het scannen van de
  8 MB PSRAM-heap overschreed de interrupt-WDT). Crashpunten bleven
  wandelen (littlefs fd-tabel, VFS-tabel tijdens ESP_LOGI) en de
  heap-controle was net vóór de crash nog geslaagd => wild-writer, geen
  klassiek heaplek. Verdachte: main-taak-stack van slechts 3584 bytes bij
  de diepe aanroepketen (demo -> component -> littlefs -> ESP_LOG) en de
  nieuwe per-stap-ringdumps.
  TEST: `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` + stack-end-watchpoint ->
  run7: demo liep volledig uit tot "example finished", alle ringdumps
  correct, validate header=1 badSlots=0 strays=0, één boot, geen resets.
  => ROOT OORZAAK: stack-overflow main-taak; de eerdere "IPC/CPU1-hang"
  en de willekeurige crashes waren symptomen van heapcorruptie door het
  oversturende stackgebruik. Host-suite (573 checks) met ASan/UBSan: 0
  fouten (component zelf is geheugendicht).
  Herstel naar productieconfiguratie: PSRAM weer aan, dual-core weer aan,
  heap-poisoning en stack-watchpoint weer uit, idle-task-WDT-checks weer
  aan; main-stack 8192 blijft (oplossing) en is vastgelegd in
  `sdkconfig.defaults.esp32s3`. Hierna: erase-flash + schone
  acceptatierun (create-pad incl. demoRingOperations).
- 17:14 — Acceptatietest geslaagd. Productieconfiguratie hersteld
  (dual-core aan, octal PSRAM aan, heap-poisoning en stack-watchpoint uit,
  idle-task-WDT-checks aan, main-stack 8192 als vaste oplossing in
  `sdkconfig.defaults.esp32s3`). Na `erase-flash` (schone LittleFS) één
  boot: create-pad volledig doorlopen (demoRingOperations met correcte
  evict: `add 400` verdringt oudste 100; update 300 -> errors=7 behoudt
  positie), daarna hourly logger, buffered staged/sync, resize 4->6,
  add 900, shrink->3, validate (header=1 committed=3 badSlots=0 strays=0),
  read-only open en unmount; laatste regel "example finished", geen
  watchdog, geen crashes. Daarna nog labels van twee ringdumps gecorrigeerd
  (evict zit bij add 400, niet bij add 50) en opnieuw gebouwd/geflasht:
  ook die build draalt volledig uit. Host-suite blijft 573 checks / 0
  failures (ook onder ASan/UBSan). README component: sectie
  "Filesystem, locking and task context" aangevuld met de eis van >= 8 KB
  taak-stack voor aanroepende taken.
