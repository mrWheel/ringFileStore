# ringFileStore — test log (host suite)

Generated: 18-11-2026 - 21:03 — host tests with POSIX shims and failure-injection.

Per line: **Command** (with data) and its **Result**; ring dumps show the key and the physical slot number per record.

| Test | checks | failures |
| --- | ---: | ---: |

## Test: arguments
- **Test result:** 12 checks, 0 new failures
| arguments | 12 | 0 |

## Test: createOpenReopen
- **Test result:** 6 checks, 0 new failures
| createOpenReopen | 6 | 0 |

## Test: emptyRing
- **Test result:** 6 checks, 0 new failures
| emptyRing | 6 | 0 |

## Test: singleSlotRing
- **Test result:** 114 checks, 0 new failures
| singleSlotRing | 114 | 0 |

## Test: duplicatesAndWraparound
- **Test result:** 20 checks, 0 new failures
| duplicatesAndWraparound | 20 | 0 |

## Test: updateSemantics
- **Test result:** 13 checks, 0 new failures
| updateSemantics | 13 | 0 |

## Test: keyTypes
- **Test result:** 58 checks, 0 new failures
| keyTypes | 58 | 0 |

## Test: iterator
- **Test result:** 39 checks, 0 new failures
| iterator | 39 | 0 |

## Test: schemaMismatch
- **Test result:** 5 checks, 0 new failures
| schemaMismatch | 5 | 0 |

## Test: readOnly
- **Test result:** 16 checks, 0 new failures
| readOnly | 16 | 0 |

## Test: staleLockBreak
- **Test result:** 8 checks, 0 new failures
| staleLockBreak | 8 | 0 |

## Test: corruption
- **Test result:** 26 checks, 0 new failures
| corruption | 26 | 0 |

## Test: clear
- **Test result:** 16 checks, 0 new failures
| clear | 16 | 0 |

## Test: resize
- **Test result:** 32 checks, 0 new failures
| resize | 32 | 0 |

## Test: resizeEmptyAndErrors
- **Test result:** 10 checks, 0 new failures
| resizeEmptyAndErrors | 10 | 0 |

## Test: resizeInterruptedStates
- **Test result:** 21 checks, 0 new failures
| resizeInterruptedStates | 21 | 0 |

## Test: failureInjectionMutations
- **Test result:** 15 checks, 0 new failures
| failureInjectionMutations | 15 | 0 |

## Test: unchangedWrites
- **Test result:** 13 checks, 0 new failures
| unchangedWrites | 13 | 0 |

## Test: wearDistribution
- **Test result:** 75 checks, 0 new failures
| wearDistribution | 75 | 0 |

## Test: bufferedMode
- **Test result:** 40 checks, 0 new failures
| bufferedMode | 40 | 0 |

## Test: bufferedOverflowFlushes
- **Test result:** 10 checks, 0 new failures
| bufferedOverflowFlushes | 10 | 0 |

## Test: multipleFiles
- **Test result:** 6 checks, 0 new failures
| multipleFiles | 6 | 0 |

## Test: validateGoodRing
- **Test result:** 12 checks, 0 new failures
| validateGoodRing | 12 | 0 |

## Test: moreRecords
- **Command:** ringFileAdd(key=100, energyWh=10, errorCount=1) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=100" (count=1 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
- **Command:** ringFileAdd(key=200, energyWh=20, errorCount=2) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=200" (count=2 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
- **Command:** ringFileAdd(key=300, energyWh=30, errorCount=3) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=300" (count=3 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
- **Command:** ringFileAdd(key=400, energyWh=40, errorCount=4) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=400" (count=4 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
- **Command:** ringFileAdd(key=500, energyWh=50, errorCount=5) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=500" (count=5 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
- **Command:** ringFileAdd(key=600, energyWh=60, errorCount=6) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=600" (count=6 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
- **Command:** ringFileAdd(key=700, energyWh=70, errorCount=7) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=700" (count=7 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
- **Command:** ringFileAdd(key=800, energyWh=80, errorCount=8) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=800" (count=8 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
- **Command:** ringFileAdd(key=900, energyWh=90, errorCount=9) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=900" (count=9 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
  - slot 8 key=900 energyWh=90 errorCount=9
- **Command:** ringFileAdd(key=1000, energyWh=100, errorCount=10) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=1000" (count=10 capacity=10, oldest → newest)
  - slot 0 key=100 energyWh=10 errorCount=1
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
  - slot 8 key=900 energyWh=90 errorCount=9
  - slot 9 key=1000 energyWh=100 errorCount=10
- **Command:** ringFileAdd(key=1100, energyWh=110, errorCount=11) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=1100" (count=10 capacity=10, oldest → newest)
  - slot 1 key=200 energyWh=20 errorCount=2
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
  - slot 8 key=900 energyWh=90 errorCount=9
  - slot 9 key=1000 energyWh=100 errorCount=10
  - slot 10 key=1100 energyWh=110 errorCount=11
- **Command:** ringFileAdd(key=1200, energyWh=120, errorCount=12) on a capacity-10 ring
  **Result:** ESP_OK
- **Command:** ring dump after "add key=1200" (count=10 capacity=10, oldest → newest)
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 4 key=500 energyWh=50 errorCount=5
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
  - slot 8 key=900 energyWh=90 errorCount=9
  - slot 9 key=1000 energyWh=100 errorCount=10
  - slot 10 key=1100 energyWh=110 errorCount=11
  - slot 0 key=1200 energyWh=120 errorCount=12
- **Command:** ringFileReadByKey(key=100) — evicted by the 11th add
  **Result:** RING_FILE_STORE_ERR_KEY_NOT_FOUND
- **Command:** ringFileReadByKey(key=200) — evicted by the 12th add
  **Result:** RING_FILE_STORE_ERR_KEY_NOT_FOUND
- **Command:** ringFileReadByKey for all 10 active keys (300..1200)
  **Result:** keys 300..1200 active, energyWh correct
- **Command:** ringFileReadOldest()
  **Result:** ESP_OK
- **Command:** ringFileReadNewest()
  **Result:** ESP_OK
- **Command:** ringFileUpdateByKey(key=500, energyWh=999, errorCount=7) — position preserved
  **Result:** ESP_OK
- **Command:** ring dump after "update key=500" (count=10 capacity=10, oldest → newest)
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 1 key=500 energyWh=999 errorCount=7
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
  - slot 8 key=900 energyWh=90 errorCount=9
  - slot 9 key=1000 energyWh=100 errorCount=10
  - slot 10 key=1100 energyWh=110 errorCount=11
  - slot 0 key=1200 energyWh=120 errorCount=12
- **Command:** ring dump after "reopen after close" (count=10 capacity=10, oldest → newest)
  - slot 2 key=300 energyWh=30 errorCount=3
  - slot 3 key=400 energyWh=40 errorCount=4
  - slot 1 key=500 energyWh=999 errorCount=7
  - slot 5 key=600 energyWh=60 errorCount=6
  - slot 6 key=700 energyWh=70 errorCount=7
  - slot 7 key=800 energyWh=80 errorCount=8
  - slot 8 key=900 energyWh=90 errorCount=9
  - slot 9 key=1000 energyWh=100 errorCount=10
  - slot 10 key=1100 energyWh=110 errorCount=11
  - slot 0 key=1200 energyWh=120 errorCount=12
- **Test result:** 43 checks, 0 new failures
| moreRecords | 43 | 0 |

## Test: keyPositionMask
- **Command:** ringFileCreate(capacity=4, key=timestamp u64, keyPositionMask=0x00000000000000fc: byte positions 0-1 not significant)
  **Result:** ESP_OK
- **Command:** ringFileAdd(key=0x0000000012340001)
  **Result:** ESP_OK
- **Command:** ringFileAdd(key=0x00000000123400ab: only bytes 0-1 differ)
  **Result:** RING_FILE_STORE_ERR_DUPLICATE_KEY
- **Command:** ringFileReadByKey(key=0x00000000123400ff) finds key=0x0000000012340001 (byte 0 ignored)
  **Result:** ESP_OK
- **Command:** ringFileReadByKey(key=0x0000000012350000): byte 2 (significant) differs
  **Result:** RING_FILE_STORE_ERR_KEY_NOT_FOUND
- **Command:** ringFileUpdateByKey(key=0x0000000012340002, embedded key 0x0000000012340003: only byte 0 differs): accepted
  **Result:** ESP_OK
- **Command:** ringFileReadByKey(key=0x0000000012340001) shows the update
  **Result:** ESP_OK
- **Command:** ring dump after "keymask scenario (adds, duplicate, update)" (count=1 capacity=4, oldest → newest)
  - slot 1 key=305397763 energyWh=33 errorCount=1
- **Command:** reopen: keyPositionMask from header, readByKey(0x00000000123400ff) finds the record
  **Result:** ESP_OK
- **Command:** ringFileCreate keyPositionMask=0x1FF (bit beyond keySize=8) -> rejected
  **Result:** ESP_ERR_INVALID_ARG
- **Command:** ringFileCreate BYTES keySize=80 with keyPositionMask (mask limited to positions 0-63) -> rejected
  **Result:** ESP_ERR_INVALID_ARG
- **Test result:** 17 checks, 0 new failures
| keyPositionMask | 17 | 0 |

## Summary

**PASS**: 633 checks, 0 failures.
