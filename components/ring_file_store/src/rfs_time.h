//--
//-- FileVersion: [1] 19-11-2026 - 09:06
//--
//-- ringFileStore - timestamp helpers for TIMEBUCKET addressing.
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

//-- Parse exactly 14 digits "EEYYMMDDHHMMSS". Century fixed to "20"
//-- (years 2000-2099), month 01-12, day valid for the month, hour 00-23,
//-- minute and second 00-59.
bool rfsTimeStampStringToUnix(const char *timeStamp, uint64_t *outUnixSeconds);

//-- Writes 14 digits plus NUL. Rejects values outside
//-- 2000-01-01 00:00:00 .. 2099-12-31 23:59:59.
bool rfsTimeUnixToTimeStampString(uint64_t unixSeconds, char outTimeStamp[15]);

//-- bucketId per granularity (see ringFileGranularity_t). Returns false
//-- for an invalid granularity value.
bool rfsTimeUnixToBucketId(uint64_t unixSeconds,
                           ringFileGranularity_t granularity,
                           uint64_t *outBucketId);

//-- ISO-8601 week-numbering year and week (weeks start Monday, week 1
//-- contains the first Thursday).
bool rfsTimeUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                          uint32_t *outIsoWeek);

#endif
