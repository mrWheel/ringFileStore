# ringFileStore — test log (host suite)

Generated: 20-11-2026 - 22:43 — host tests with POSIX shims and failure-injection.

Every stored record carries the automatic key fields timeStamp and usedKey; ring dumps show both plus the reported bucketId (period % capacity).

| Test | checks | failures |
| --- | ---: | ---: |

## Test: arguments
- **Test result:** 31 checks, 0 new failures
| arguments | 31 | 0 |

## Test: createOpenReopen
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=100)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=101)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=102)
  **Result:** ESP_OK
- **Command:** ring dump after "add hh0..hh2" (count=3 capacity=4, oldest → newest)
  - bucketId 0 ts=20261023000000 usedKey=2026102300 energyWh=100 errorCount=0
  - bucketId 1 ts=20261023010000 usedKey=2026102301 energyWh=101 errorCount=1
  - bucketId 2 ts=20261023020000 usedKey=2026102302 energyWh=102 errorCount=2
- **Test result:** 25 checks, 0 new failures
| createOpenReopen | 25 | 0 |

## Test: emptyRing
- **Test result:** 7 checks, 0 new failures
| emptyRing | 7 | 0 |

## Test: singleSlotRing
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=20)
  **Result:** ESP_OK
- **Test result:** 12 checks, 0 new failures
| singleSlotRing | 12 | 0 |

## Test: sameBucketUpdate
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=100)
  **Result:** ESP_OK
- **Command:** ring dump after "add hh1" (count=1 capacity=3, oldest → newest)
  - bucketId 1 ts=20261023010000 usedKey=2026102301 energyWh=100 errorCount=1
- **Command:** ring dump after "upsert hh1:30" (count=1 capacity=3, oldest → newest)
  - bucketId 1 ts=20261023013000 usedKey=2026102301 energyWh=150 errorCount=3
- **Test result:** 8 checks, 0 new failures
| sameBucketUpdate | 8 | 0 |

## Test: notNewer
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=30)
  **Result:** ESP_OK
- **Command:** ring dump after "add hh0..hh2 (capacity 2, hh0 evicted)" (count=2 capacity=2, oldest → newest)
  - bucketId 1 ts=20261023010000 usedKey=2026102301 energyWh=20 errorCount=1
  - bucketId 0 ts=20261023020000 usedKey=2026102302 energyWh=30 errorCount=2
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=99)
  **Result:** UNKNOWN
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=25)
  **Result:** ESP_OK
- **Test result:** 9 checks, 0 new failures
| notNewer | 9 | 0 |

## Test: timestampValidation
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=7)
  **Result:** ESP_OK
- **Test result:** 12 checks, 0 new failures
| timestampValidation | 12 | 0 |

## Test: bucketAndKeyDerivation
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=100)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=101)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=102)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=103)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=104)
  **Result:** ESP_OK
- **Command:** ring dump after "add hh0..hh4 (capacity 5)" (count=5 capacity=5, oldest → newest)
  - bucketId 1 ts=20261023000000 usedKey=2026102300 energyWh=100 errorCount=0
  - bucketId 2 ts=20261023010000 usedKey=2026102301 energyWh=101 errorCount=1
  - bucketId 3 ts=20261023020000 usedKey=2026102302 energyWh=102 errorCount=2
  - bucketId 4 ts=20261023030000 usedKey=2026102303 energyWh=103 errorCount=3
  - bucketId 0 ts=20261023040000 usedKey=2026102304 energyWh=104 errorCount=4
- **Command:** ringFileAdd(months 202610, 202611)
  **Result:** ESP_OK
- **Test result:** 99 checks, 0 new failures
| bucketAndKeyDerivation | 99 | 0 |

## Test: evictionOrder
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=0)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=30)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=40)
  **Result:** ESP_OK
- **Command:** ring dump after "add hh0..hh4 (capacity 3)" (count=3 capacity=3, oldest → newest)
  - bucketId 2 ts=20261023020000 usedKey=2026102302 energyWh=20 errorCount=2
  - bucketId 0 ts=20261023030000 usedKey=2026102303 energyWh=30 errorCount=3
  - bucketId 1 ts=20261023040000 usedKey=2026102304 energyWh=40 errorCount=4
- **Test result:** 13 checks, 0 new failures
| evictionOrder | 13 | 0 |

## Test: editWindow
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** ring dump after "edit hh5 insert (evicts hh0)" (count=3 capacity=3, oldest → newest)
  - bucketId 1 ts=20261023010000 usedKey=2026102301 energyWh=11 errorCount=1
  - bucketId 2 ts=20261023024500 usedKey=2026102302 energyWh=77 errorCount=0
  - bucketId 2 ts=20261023050000 usedKey=2026102305 energyWh=55 errorCount=0
- **Test result:** 14 checks, 0 new failures
| editWindow | 14 | 0 |

## Test: updateByKeyRules
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=100)
  **Result:** ESP_OK
- **Test result:** 11 checks, 0 new failures
| updateByKeyRules | 11 | 0 |

## Test: iterator
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=13)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=14)
  **Result:** ESP_OK
- **Test result:** 36 checks, 0 new failures
| iterator | 36 | 0 |

## Test: schemaMismatch
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Test result:** 6 checks, 0 new failures
| schemaMismatch | 6 | 0 |

## Test: readOnly
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Test result:** 10 checks, 0 new failures
| readOnly | 10 | 0 |

## Test: staleLockBreak
- **Test result:** 5 checks, 0 new failures
| staleLockBreak | 5 | 0 |

## Test: corruption
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Test result:** 18 checks, 0 new failures
| corruption | 18 | 0 |

## Test: clear
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023070000 usedKey=2026102307 energyWh=70)
  **Result:** ESP_OK
- **Test result:** 11 checks, 0 new failures
| clear | 11 | 0 |

## Test: resize
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=0)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=30)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=40)
  **Result:** ESP_OK
- **Command:** ring dump after "resize 5 -> 3" (count=3 capacity=3, oldest → newest)
  - bucketId 2 ts=20261023020000 usedKey=2026102302 energyWh=20 errorCount=2
  - bucketId 0 ts=20261023030000 usedKey=2026102303 energyWh=30 errorCount=3
  - bucketId 1 ts=20261023040000 usedKey=2026102304 energyWh=40 errorCount=4
- **Command:** ringFileAdd(ts=20261023050000 usedKey=2026102305 energyWh=50)
  **Result:** ESP_OK
- **Test result:** 25 checks, 0 new failures
| resize | 25 | 0 |

## Test: resizeInterruptedStates
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** resize 5->3 with injected failure (scenario 0)
  **Result:** rejected
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** resize 5->3 with injected failure (scenario 1)
  **Result:** rejected
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** resize 5->3 with injected failure (scenario 2)
  **Result:** rejected
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** resize 5->3 with injected failure (scenario 3)
  **Result:** rejected
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Command:** resize 5->3 with injected failure (scenario 4)
  **Result:** rejected
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Test result:** 39 checks, 0 new failures
| resizeInterruptedStates | 39 | 0 |

## Test: failureInjectionMutations
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=20)
  **Result:** RING_FILE_STORE_ERR_IO
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=30)
  **Result:** ESP_OK
- **Test result:** 11 checks, 0 new failures
| failureInjectionMutations | 11 | 0 |

## Test: unchangedWrites
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=100)
  **Result:** ESP_OK
- **Test result:** 9 checks, 0 new failures
| unchangedWrites | 9 | 0 |

## Test: wearDistribution
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=0)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=30)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=40)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023050000 usedKey=2026102305 energyWh=50)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023060000 usedKey=2026102306 energyWh=60)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023070000 usedKey=2026102307 energyWh=70)
  **Result:** ESP_OK
- **Test result:** 14 checks, 0 new failures
| wearDistribution | 14 | 0 |

## Test: bufferedMode
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=30)
  **Result:** ESP_OK
- **Command:** ring dump after "buffered 3 staged" (count=3 capacity=4, oldest → newest)
  - slot **staged** (not yet on storage) ts=20261023000000 usedKey=2026102300 energyWh=10 errorCount=0
  - slot **staged** (not yet on storage) ts=20261023010000 usedKey=2026102301 energyWh=20 errorCount=1
  - slot **staged** (not yet on storage) ts=20261023020000 usedKey=2026102302 energyWh=30 errorCount=2
- **Test result:** 22 checks, 0 new failures
| bufferedMode | 22 | 0 |

## Test: bufferedOverflowFlushes
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=20)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=30)
  **Result:** ESP_OK
- **Test result:** 7 checks, 0 new failures
| bufferedOverflowFlushes | 7 | 0 |

## Test: multipleTypes
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=100)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=101)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=102)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=103)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=104)
  **Result:** ESP_OK
- **Test result:** 35 checks, 0 new failures
| multipleTypes | 35 | 0 |

## Test: validateGoodRing
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=10)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=12)
  **Result:** ESP_OK
- **Test result:** 18 checks, 0 new failures
| validateGoodRing | 18 | 0 |

## Test: manyRecords
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=1000)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=1001)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=1002)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=1003)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=1004)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023050000 usedKey=2026102305 energyWh=1005)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023060000 usedKey=2026102306 energyWh=1006)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023070000 usedKey=2026102307 energyWh=1007)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023080000 usedKey=2026102308 energyWh=1008)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023090000 usedKey=2026102309 energyWh=1009)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024000000 usedKey=2026102400 energyWh=1010)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024010000 usedKey=2026102401 energyWh=1011)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024020000 usedKey=2026102402 energyWh=1012)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024030000 usedKey=2026102403 energyWh=1013)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024040000 usedKey=2026102404 energyWh=1014)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024050000 usedKey=2026102405 energyWh=1015)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024060000 usedKey=2026102406 energyWh=1016)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024070000 usedKey=2026102407 energyWh=1017)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024080000 usedKey=2026102408 energyWh=1018)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024090000 usedKey=2026102409 energyWh=1019)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025000000 usedKey=2026102500 energyWh=1020)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025010000 usedKey=2026102501 energyWh=1021)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025020000 usedKey=2026102502 energyWh=1022)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025030000 usedKey=2026102503 energyWh=1023)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025040000 usedKey=2026102504 energyWh=1024)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025050000 usedKey=2026102505 energyWh=1025)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025060000 usedKey=2026102506 energyWh=1026)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025070000 usedKey=2026102507 energyWh=1027)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025080000 usedKey=2026102508 energyWh=1028)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025090000 usedKey=2026102509 energyWh=1029)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026000000 usedKey=2026102600 energyWh=1030)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026010000 usedKey=2026102601 energyWh=1031)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026020000 usedKey=2026102602 energyWh=1032)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026030000 usedKey=2026102603 energyWh=1033)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026040000 usedKey=2026102604 energyWh=1034)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026050000 usedKey=2026102605 energyWh=1035)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026060000 usedKey=2026102606 energyWh=1036)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026070000 usedKey=2026102607 energyWh=1037)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026080000 usedKey=2026102608 energyWh=1038)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026090000 usedKey=2026102609 energyWh=1039)
  **Result:** ESP_OK
- **Test result:** 79 checks, 0 new failures
| manyRecords | 79 | 0 |

## Test: directoryCache
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=2000)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=2001)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023020000 usedKey=2026102302 energyWh=2002)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023030000 usedKey=2026102303 energyWh=2003)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023040000 usedKey=2026102304 energyWh=2004)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023050000 usedKey=2026102305 energyWh=2005)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023060000 usedKey=2026102306 energyWh=2006)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023070000 usedKey=2026102307 energyWh=2007)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023080000 usedKey=2026102308 energyWh=2008)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023090000 usedKey=2026102309 energyWh=2009)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024000000 usedKey=2026102400 energyWh=2010)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024010000 usedKey=2026102401 energyWh=2011)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024020000 usedKey=2026102402 energyWh=2012)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024030000 usedKey=2026102403 energyWh=2013)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024040000 usedKey=2026102404 energyWh=2014)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024050000 usedKey=2026102405 energyWh=2015)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024060000 usedKey=2026102406 energyWh=2016)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024070000 usedKey=2026102407 energyWh=2017)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024080000 usedKey=2026102408 energyWh=2018)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261024090000 usedKey=2026102409 energyWh=2019)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025000000 usedKey=2026102500 energyWh=2020)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025010000 usedKey=2026102501 energyWh=2021)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025020000 usedKey=2026102502 energyWh=2022)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025030000 usedKey=2026102503 energyWh=2023)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025040000 usedKey=2026102504 energyWh=2024)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025050000 usedKey=2026102505 energyWh=2025)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025060000 usedKey=2026102506 energyWh=2026)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025070000 usedKey=2026102507 energyWh=2027)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025080000 usedKey=2026102508 energyWh=2028)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261025090000 usedKey=2026102509 energyWh=2029)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026000000 usedKey=2026102600 energyWh=2030)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026010000 usedKey=2026102601 energyWh=2031)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026020000 usedKey=2026102602 energyWh=2032)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026030000 usedKey=2026102603 energyWh=2033)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026040000 usedKey=2026102604 energyWh=2034)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026050000 usedKey=2026102605 energyWh=2035)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026060000 usedKey=2026102606 energyWh=2036)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026070000 usedKey=2026102607 energyWh=2037)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026080000 usedKey=2026102608 energyWh=2038)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026090000 usedKey=2026102609 energyWh=2039)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261026100000 usedKey=2026102610 energyWh=2333)
  **Result:** ESP_OK
- **Test result:** 54 checks, 0 new failures
| directoryCache | 54 | 0 |

## Test: crossHandleCache
- **Command:** ringFileAdd(ts=20261023000000 usedKey=2026102300 energyWh=11)
  **Result:** ESP_OK
- **Command:** ringFileAdd(ts=20261023010000 usedKey=2026102301 energyWh=12)
  **Result:** ESP_OK
- **Test result:** 17 checks, 0 new failures
| crossHandleCache | 17 | 0 |

## Test: timeHelpers
- **Test result:** 32 checks, 0 new failures
| timeHelpers | 32 | 0 |

## Summary

**PASS**: 689 checks, 0 failures.
