//--
//-- FileVersion: [4] 19-11-2026 - 15:25
//--
//-- ringFileStore - timestamp helpers for the timestamp key model.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "rfs_time.h"

//---------------------------------------------------------------------
//-- Time basis: the local wall clock is treated as if it were UTC;
//-- daylight saving time is ignored. The numeric timestamp form is the
//-- 14-digit EEYYMMDDhhmmss value with the century fixed to "20" (years
//-- 2000-2099), back-filled with "00" values below the file type. Period
//-- formulas (days = days since 1970-01-01):
//--   YEAR    year
//--   MONTH   (year - 1) * 12 + month
//--   DAY     days
//--   HOUR    days * 24 + hour
//--   MINUTE  days * 1440 + hour * 60 + minute
//--   SECOND  days * 86400 + hour * 3600 + minute * 60 + second
//-- bucketId (the logical slot) is period % capacity.
//-- "days" counts only the COMPLETE days before the date (days_from_civil
//-- maps 1970-01-01 to 0), so a partial current period never counts as a
//-- full unit: on day D the hourly period at 14:xx is D * 24 + 14 — the
//-- current day contributes its completed hours only, never the full 24.
//-- The day/minute/second formulas behave the same way. The month formula
//-- counts the months of the previous years plus the index of the current
//-- month: (year - 1) * 12 + month.
//---------------------------------------------------------------------

#define RFS_TIME_SECONDS_PER_DAY 86400ull
#define RFS_TIME_SECONDS_PER_HOUR 3600ull
#define RFS_TIME_SECONDS_PER_MINUTE 60ull
#define RFS_TIME_WEEK_OFFSET_SECONDS 259200ull
#define RFS_TIME_WEEK_LENGTH_SECONDS 604800ull
#define RFS_TIME_UNIX_2000_01_01 946684800ull
#define RFS_TIME_UNIX_2099_12_31_END 4102444799ull
#define RFS_TIME_STAMP_LEN 14u

//-- Days in month m (1..12) of civil year y (proleptic Gregorian).
static uint32_t rfsTimeDaysInMonth(int64_t y, uint32_t m)
{
  static const uint32_t days[12] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
  if (m == 2 && (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0)))
  {
    return 29;
  }

  return days[m - 1u];
}

bool rfsTimeDaysFromCivil(int64_t year, uint32_t month, uint32_t day,
                          int64_t *outDays)
{
  if (outDays == NULL || month < 1u || month > 12u || year < 1)
  {
    return false;
  }

  if (day < 1u || day > rfsTimeDaysInMonth(year, month))
  {
    return false;
  }

  //-- Howard Hinnant's days_from_civil.
  int64_t y = year - ((month <= 2u) ? 1 : 0);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;                                   //-- [0, 399]
  int64_t mp = (int64_t)month + ((month > 2u) ? -3 : 9);         //-- [0, 11]
  int64_t doy = (153 * mp + 2) / 5 + (int64_t)day - 1;           //-- [0, 365]
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           //-- [0, 146096]

  *outDays = era * 146097 + doe - 719468;
  return true;
}

void rfsTimeCivilFromDays(int64_t days, int64_t *outYear, uint32_t *outMonth,
                          uint32_t *outDay)
{
  //-- Howard Hinnant's civil_from_days.
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;                                //-- [0, 146096]
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);         //-- [0, 365]
  int64_t mp = (5 * doy + 2) / 153;                              //-- [0, 11]
  int64_t d = doy - (153 * mp + 2) / 5 + 1;                      //-- [1, 31]
  int64_t m = mp + ((mp < 10) ? 3 : -9);                         //-- [1, 12]

  y += ((m <= 2) ? 1 : 0);

  *outYear = y;
  *outMonth = (uint32_t)m;
  *outDay = (uint32_t)d;
}

bool rfsTimeStampStringToUnix(const char *timeStamp, uint64_t *outUnixSeconds)
{
  if (timeStamp == NULL || outUnixSeconds == NULL)
  {
    return false;
  }

  if (strlen(timeStamp) != RFS_TIME_STAMP_LEN)
  {
    return false;
  }

  for (size_t i = 0; i < RFS_TIME_STAMP_LEN; i++)
  {
    if (timeStamp[i] < '0' || timeStamp[i] > '9')
    {
      return false;
    }
  }

  //-- Century is fixed to "20": only years 2000-2099 are representable.
  if (timeStamp[0] != '2' || timeStamp[1] != '0')
  {
    return false;
  }

  uint32_t digits[RFS_TIME_STAMP_LEN];

  for (size_t i = 0; i < RFS_TIME_STAMP_LEN; i++)
  {
    digits[i] = (uint32_t)(timeStamp[i] - '0');
  }

  int64_t year = 2000 + (int64_t)(digits[2] * 10u + digits[3]);
  uint32_t month = digits[4] * 10u + digits[5];
  uint32_t day = digits[6] * 10u + digits[7];
  uint32_t hour = digits[8] * 10u + digits[9];
  uint32_t minute = digits[10] * 10u + digits[11];
  uint32_t second = digits[12] * 10u + digits[13];

  if (hour > 23u || minute > 59u || second > 59u)
  {
    return false;
  }

  int64_t days = 0;

  if (!rfsTimeDaysFromCivil(year, month, day, &days))
  {
    return false;
  }

  *outUnixSeconds = (uint64_t)days * RFS_TIME_SECONDS_PER_DAY +
                    (uint64_t)hour * RFS_TIME_SECONDS_PER_HOUR +
                    (uint64_t)minute * RFS_TIME_SECONDS_PER_MINUTE +
                    (uint64_t)second;
  return true;
}

bool rfsTimeUnixToTimeStampString(uint64_t unixSeconds, char outTimeStamp[15])
{
  if (outTimeStamp == NULL)
  {
    return false;
  }

  if (unixSeconds < RFS_TIME_UNIX_2000_01_01 ||
      unixSeconds > RFS_TIME_UNIX_2099_12_31_END)
  {
    return false;
  }

  int64_t days = (int64_t)(unixSeconds / RFS_TIME_SECONDS_PER_DAY);
  uint64_t rest = unixSeconds % RFS_TIME_SECONDS_PER_DAY;
  uint32_t hour = (uint32_t)(rest / RFS_TIME_SECONDS_PER_HOUR);
  uint32_t minute =
      (uint32_t)((rest % RFS_TIME_SECONDS_PER_HOUR) / RFS_TIME_SECONDS_PER_MINUTE);
  uint32_t second = (uint32_t)(rest % RFS_TIME_SECONDS_PER_MINUTE);

  int64_t year = 0;
  uint32_t month = 0, day = 0;
  rfsTimeCivilFromDays(days, &year, &month, &day);

  if (year < 2000 || year > 2099)
  {
    return false;
  }

  //-- "20" century + yy mm dd hh mm ss = 14 digits. (unsigned) casts keep
  //-- the %u conversion portable (uint32_t is 'unsigned int' on the host
  //-- but 'unsigned long' on xtensa).
  snprintf(outTimeStamp, 15, "20%02u%02u%02u%02u%02u%02u",
           (unsigned)(year - 2000), (unsigned)month, (unsigned)day,
           (unsigned)hour, (unsigned)minute, (unsigned)second);
  return true;
}

bool rfsTimeUnixToBucketId(uint64_t unixSeconds,
                           ringFileGranularity_t granularity,
                           uint64_t *outPeriod)
{
  if (outPeriod == NULL ||
      unixSeconds > RFS_TIME_UNIX_2099_12_31_END)
  {
    return false;
  }

  int64_t days = (int64_t)(unixSeconds / RFS_TIME_SECONDS_PER_DAY);
  uint64_t rest = unixSeconds % RFS_TIME_SECONDS_PER_DAY;
  uint32_t hour = (uint32_t)(rest / RFS_TIME_SECONDS_PER_HOUR);
  uint32_t minute =
      (uint32_t)((rest % RFS_TIME_SECONDS_PER_HOUR) / RFS_TIME_SECONDS_PER_MINUTE);
  uint32_t second = (uint32_t)(rest % RFS_TIME_SECONDS_PER_MINUTE);
  int64_t year = 0;
  uint32_t month = 0, day = 0;
  rfsTimeCivilFromDays(days, &year, &month, &day);

  switch (granularity)
  {
    case RING_FILE_GRANULARITY_YEAR:
      *outPeriod = (uint64_t)year;
      return true;

    case RING_FILE_GRANULARITY_MONTH:
      //-- Month formula (before the modulo addressing).
      *outPeriod = (uint64_t)(year - 1) * 12u + (uint64_t)month;
      return true;

    case RING_FILE_GRANULARITY_DAY:
      *outPeriod = (uint64_t)days;
      return true;

    case RING_FILE_GRANULARITY_HOUR:
      *outPeriod = (uint64_t)days * 24u + (uint64_t)hour;
      return true;

    case RING_FILE_GRANULARITY_MINUTE:
      *outPeriod = (uint64_t)days * 1440u + (uint64_t)hour * 60u +
                   (uint64_t)minute;
      return true;

    case RING_FILE_GRANULARITY_SECOND:
      *outPeriod = (uint64_t)days * RFS_TIME_SECONDS_PER_DAY +
                   (uint64_t)hour * RFS_TIME_SECONDS_PER_HOUR +
                   (uint64_t)minute * RFS_TIME_SECONDS_PER_MINUTE +
                   (uint64_t)second;
      return true;

    default:
      return false;
  }
}

//-- ---------------------------------------------------------------------------
//-- Digit-based helpers behind the timestamp key model.
//-- ---------------------------------------------------------------------------

//-- Digit divisors for the truncation to a usedKey (10 ^ digits below the
//-- granularity) and for padding a usedKey back to 14 digits.
static bool rfsTimeGranularityDivisors(ringFileGranularity_t granularity,
                                       uint64_t *outTruncateDivisor,
                                       uint64_t *outPadDivisor)
{
  switch (granularity)
  {
    //-- A 14-digit EEYYMMDDhhmmss value: the usedKey keeps 4 + 2*k leading
    //-- digits, so truncate = pad = 10^(10 - 2*k).
    case RING_FILE_GRANULARITY_YEAR:
      *outTruncateDivisor = 10000000000ull;
      *outPadDivisor = 10000000000ull;
      return true;

    case RING_FILE_GRANULARITY_MONTH:
      *outTruncateDivisor = 100000000ull;
      *outPadDivisor = 100000000ull;
      return true;

    case RING_FILE_GRANULARITY_DAY:
      *outTruncateDivisor = 1000000ull;
      *outPadDivisor = 1000000ull;
      return true;

    case RING_FILE_GRANULARITY_HOUR:
      *outTruncateDivisor = 10000ull;
      *outPadDivisor = 10000ull;
      return true;

    case RING_FILE_GRANULARITY_MINUTE:
      *outTruncateDivisor = 100ull;
      *outPadDivisor = 100ull;
      return true;

    case RING_FILE_GRANULARITY_SECOND:
      *outTruncateDivisor = 1ull;
      *outPadDivisor = 1ull;
      return true;

    default:
      return false;
  }
}

bool rfsTimeSchemaIdToGranularity(const char *schemaId,
                                  ringFileGranularity_t *outGranularity)
{
  if (schemaId == NULL || outGranularity == NULL)
  {
    return false;
  }

  if (strcmp(schemaId, RING_FILE_SCHEMA_YY) == 0)
  {
    *outGranularity = RING_FILE_GRANULARITY_YEAR;
    return true;
  }

  if (strcmp(schemaId, RING_FILE_SCHEMA_YYMM) == 0)
  {
    *outGranularity = RING_FILE_GRANULARITY_MONTH;
    return true;
  }

  if (strcmp(schemaId, RING_FILE_SCHEMA_YYMMDD) == 0)
  {
    *outGranularity = RING_FILE_GRANULARITY_DAY;
    return true;
  }

  if (strcmp(schemaId, RING_FILE_SCHEMA_YYMMDDHH) == 0)
  {
    *outGranularity = RING_FILE_GRANULARITY_HOUR;
    return true;
  }

  if (strcmp(schemaId, RING_FILE_SCHEMA_YYMMDDHHMM) == 0)
  {
    *outGranularity = RING_FILE_GRANULARITY_MINUTE;
    return true;
  }

  if (strcmp(schemaId, RING_FILE_SCHEMA_YYMMDDHHMMSS) == 0)
  {
    *outGranularity = RING_FILE_GRANULARITY_SECOND;
    return true;
  }

  return false;
}

bool rfsTimeDigitsValid(uint64_t timeStamp, ringFileGranularity_t granularity)
{
  if (granularity < RING_FILE_GRANULARITY_YEAR ||
      granularity > RING_FILE_GRANULARITY_SECOND)
  {
    return false;
  }

  //-- Exactly 14 digits with the century fixed to "20".
  if (timeStamp < 20000000000000ull || timeStamp > 20999999999999ull)
  {
    return false;
  }

  uint32_t year = (uint32_t)(timeStamp / 10000000000ull);
  uint32_t month = (uint32_t)((timeStamp / 100000000ull) % 100ull);
  uint32_t day = (uint32_t)((timeStamp / 1000000ull) % 100ull);
  uint32_t hour = (uint32_t)((timeStamp / 10000ull) % 100ull);
  uint32_t minute = (uint32_t)((timeStamp / 100ull) % 100ull);
  uint32_t second = (uint32_t)(timeStamp % 100ull);

  if (hour > 23u || minute > 59u || second > 59u)
  {
    return false;
  }

  //-- Fields at or above the granularity must be valid; fields below it
  //-- accept zero padding or a valid value.
  if (granularity >= RING_FILE_GRANULARITY_MONTH)
  {
    if (month < 1u || month > 12u)
    {
      return false;
    }
  }
  else if (month > 12u)
  {
    return false;
  }

  int64_t days = 0;
  bool dayValid = rfsTimeDaysFromCivil((int64_t)year, month == 0u ? 1u : month,
                                       day == 0u ? 1u : day, &days);

  if (granularity >= RING_FILE_GRANULARITY_DAY)
  {
    if (day < 1u || !rfsTimeDaysFromCivil((int64_t)year, month, day, &days))
    {
      return false;
    }
  }
  else if (day != 0u && !dayValid)
  {
    return false;
  }

  return true;
}

bool rfsTimeDigitsToUsedKey(uint64_t timeStamp,
                            ringFileGranularity_t granularity,
                            uint64_t *outUsedKey)
{
  uint64_t truncate = 0;
  uint64_t pad = 0;

  if (outUsedKey == NULL || !rfsTimeGranularityDivisors(granularity, &truncate, &pad) ||
      !rfsTimeDigitsValid(timeStamp, granularity))
  {
    return false;
  }

  *outUsedKey = timeStamp / truncate;
  return true;
}

bool rfsTimeDigitsToPeriod(uint64_t timeStamp,
                           ringFileGranularity_t granularity,
                           uint64_t *outPeriod)
{
  if (outPeriod == NULL || !rfsTimeDigitsValid(timeStamp, granularity))
  {
    return false;
  }

  uint32_t year = (uint32_t)(timeStamp / 10000000000ull);
  uint32_t month = (uint32_t)((timeStamp / 100000000ull) % 100ull);
  uint32_t day = (uint32_t)((timeStamp / 1000000ull) % 100ull);
  uint32_t hour = (uint32_t)((timeStamp / 10000ull) % 100ull);
  uint32_t minute = (uint32_t)((timeStamp / 100ull) % 100ull);
  uint32_t second = (uint32_t)(timeStamp % 100ull);

  switch (granularity)
  {
    case RING_FILE_GRANULARITY_YEAR:
      *outPeriod = (uint64_t)year;
      return true;

    case RING_FILE_GRANULARITY_MONTH:
      //-- Month formula (before the modulo addressing).
      *outPeriod = (uint64_t)(year - 1) * 12u + (uint64_t)month;
      return true;

    default:
      break;
  }

  int64_t days = 0;

  if (!rfsTimeDaysFromCivil((int64_t)year, month, day, &days))
  {
    return false;
  }

  switch (granularity)
  {
    case RING_FILE_GRANULARITY_DAY:
      *outPeriod = (uint64_t)days;
      return true;

    case RING_FILE_GRANULARITY_HOUR:
      *outPeriod = (uint64_t)days * 24u + (uint64_t)hour;
      return true;

    case RING_FILE_GRANULARITY_MINUTE:
      *outPeriod = (uint64_t)days * 1440u + (uint64_t)hour * 60u +
                   (uint64_t)minute;
      return true;

    case RING_FILE_GRANULARITY_SECOND:
      *outPeriod = (uint64_t)days * RFS_TIME_SECONDS_PER_DAY +
                   (uint64_t)hour * RFS_TIME_SECONDS_PER_HOUR +
                   (uint64_t)minute * RFS_TIME_SECONDS_PER_MINUTE +
                   (uint64_t)second;
      return true;

    default:
      return false;
  }
}

bool rfsTimeUsedKeyToPeriod(uint64_t usedKey,
                            ringFileGranularity_t granularity,
                            uint64_t *outPeriod)
{
  uint64_t truncate = 0;
  uint64_t pad = 0;

  if (outPeriod == NULL || !rfsTimeGranularityDivisors(granularity, &truncate, &pad))
  {
    return false;
  }

  return rfsTimeDigitsToPeriod(usedKey * pad, granularity, outPeriod);
}

bool rfsTimeUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                          uint32_t *outIsoWeek)
{
  if (outIsoYear == NULL || outIsoWeek == NULL)
  {
    return false;
  }

  int64_t days = (int64_t)(unixSeconds / RFS_TIME_SECONDS_PER_DAY);

  //-- ISO weekday: 0 = Monday .. 6 = Sunday (1970-01-01 was a Thursday).
  int64_t isoWd = (days + 3) % 7;
  int64_t thursday = days + 3 - isoWd;

  //-- The ISO year is the calendar year of the Thursday of this week.
  int64_t isoYear = 0;
  uint32_t month = 0, day = 0;
  rfsTimeCivilFromDays(thursday, &isoYear, &month, &day);

  int64_t janFirst = 0;

  if (!rfsTimeDaysFromCivil(isoYear, 1u, 1u, &janFirst))
  {
    return false;
  }

  int64_t ordinal = days - janFirst + 1;   //-- 1..371 within the ISO year
  int64_t week = (ordinal - (isoWd + 1) + 10) / 7;

  if (week < 1 || week > 53)
  {
    return false;
  }

  *outIsoYear = (uint32_t)isoYear;
  *outIsoWeek = (uint32_t)week;
  return true;
}

//-- ---------------------------------------------------------------------------
//-- Public API wrappers (declared in ring_file_store.h).
//-- ---------------------------------------------------------------------------

esp_err_t ringFileTimeStampStringToUnix(const char *timeStamp,
                                        uint64_t *outUnixSeconds)
{
  if (!rfsTimeStampStringToUnix(timeStamp, outUnixSeconds))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t ringFileUnixToTimeStampString(uint64_t unixSeconds,
                                        char outTimeStamp[15])
{
  if (!rfsTimeUnixToTimeStampString(unixSeconds, outTimeStamp))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t ringFileUnixToBucketId(uint64_t unixSeconds,
                                 ringFileGranularity_t granularity,
                                 uint64_t *outPeriod)
{
  if (!rfsTimeUnixToBucketId(unixSeconds, granularity, outPeriod))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t ringFileGranularityFromSchemaId(const char *schemaId,
                                          ringFileGranularity_t *outGranularity)
{
  if (!rfsTimeSchemaIdToGranularity(schemaId, outGranularity))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t ringFileTimeStampToUsedKey(uint64_t timeStamp,
                                     ringFileGranularity_t granularity,
                                     uint64_t *outUsedKey)
{
  if (!rfsTimeDigitsToUsedKey(timeStamp, granularity, outUsedKey))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t ringFileTimeStampToPeriod(uint64_t timeStamp,
                                    ringFileGranularity_t granularity,
                                    uint64_t *outPeriod)
{
  if (!rfsTimeDigitsToPeriod(timeStamp, granularity, outPeriod))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}

esp_err_t ringFileUnixToIsoWeek(uint64_t unixSeconds, uint32_t *outIsoYear,
                                uint32_t *outIsoWeek)
{
  if (!rfsTimeUnixToIsoWeek(unixSeconds, outIsoYear, outIsoWeek))
  {
    return ESP_ERR_INVALID_ARG;
  }

  return ESP_OK;
}
