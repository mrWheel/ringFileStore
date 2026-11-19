//--
//-- FileVersion: [2] 19-11-2026 - 11:40
//--
//-- ringFileStore - timestamp helpers for the timestamp key model.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//--
#ifndef RFS_TIME_H
#define RFS_TIME_H

#include <stdbool.h>
#include <stdint.h>

#include "ring_file_store.h"

//-- ---------------------------------------------------------------------------
//-- Civil calendar helpers (proleptic Gregorian, days relative to
//-- 1970-01-01). Howard Hinnant's days_from_civil / civil_from_days.
//-- ---------------------------------------------------------------------------

//-- Days since 1970-01-01 for a civil date. Validates month 1..12 and day
//-- against the calendar month (leap years included). Returns false on an
//-- invalid date. Years must be >= 1 (the ring's 2000..2099 window never
//-- approaches the formula's edge cases).
bool rfsTimeDaysFromCivil(int64_t year, uint32_t month, uint32_t day,
                          int64_t *outDays);

//-- Inverse of rfsTimeDaysFromCivil(). Always succeeds.
void rfsTimeCivilFromDays(int64_t days, int64_t *outYear, uint32_t *outMonth,
                          uint32_t *outDay);

//-- ---------------------------------------------------------------------------
//-- Internal helpers behind the public ringFileTimeStamp*/ringFileUnix* API.
//-- The boolean forms return false on invalid arguments so the core can use
//-- them without esp_err_t mapping.
//-- ---------------------------------------------------------------------------

//-- Map a file type string ("YY".."YYMMDDhhmmss") to its granularity.
bool rfsTimeSchemaIdToGranularity(const char *schemaId,
                                  ringFileGranularity_t *outGranularity);

//-- Validate a 14-digit EEYYMMDDhhmmss numeric timestamp for a
//-- granularity. Century must be "20". Fields at or above the granularity
//-- must be valid (month 01-12, day valid for the month, hour 00-23,
//-- minute/second 00-59); fields below the granularity accept zero
//-- padding or a valid value.
bool rfsTimeDigitsValid(uint64_t timeStamp, ringFileGranularity_t granularity);

//-- usedKey: the timestamp truncated to the granularity digits, e.g.
//-- 20261118143055 with HOUR gives 2026111814. Validates the digits.
bool rfsTimeDigitsToUsedKey(uint64_t timeStamp,
                            ringFileGranularity_t granularity,
                            uint64_t *outUsedKey);

//-- Period ("berekende waarde") from a validated 14-digit timestamp
//-- (validates first). See ringFileGranularity_t for the formulas.
bool rfsTimeDigitsToPeriod(uint64_t timeStamp,
                           ringFileGranularity_t granularity,
                           uint64_t *outPeriod);

//-- Period from a usedKey (zero-pads it back to 14 digits, then applies
//-- rfsTimeDigitsToPeriod()). Returns false for a malformed usedKey.
bool rfsTimeUsedKeyToPeriod(uint64_t usedKey,
                            ringFileGranularity_t granularity,
                            uint64_t *outPeriod);

//-- Parse exactly 14 digits "EEYYMMDDHHMMSS". Century fixed to "20"
//-- (years 2000-2099), month 01-12, day valid for the month, hour 00-23,
//-- minute and second 00-59.
bool rfsTimeStampStringToUnix(const char *timeStamp, uint64_t *outUnixSeconds);

//-- Writes 14 digits plus NUL. Rejects values outside
//-- 2000-01-01 00:00:00 .. 2099-12-31 23:59:59.
bool rfsTimeUnixToTimeStampString(uint64_t unixSeconds, char outTimeStamp[15]);

//-- Period per granularity from unix seconds (see ringFileGranularity_t).
//-- Returns false for an invalid granularity value.
bool rfsTimeUnixToBucketId(uint64_t unixSeconds,
                           ringFileGranularity_t granularity,
                           uint64_t *outPeriod);

//-- ISO-8601 week-numbering year and week (weeks start Monday, week 1
//-- contains the first Thursday).
bool rfsTimeUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                          uint32_t *outIsoWeek);

#endif
