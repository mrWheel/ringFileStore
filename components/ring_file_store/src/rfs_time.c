//--
//-- FileVersion: [1] 19-11-2026 - 09:06
//--
//-- ringFileStore - timestamp helpers for TIMEBUCKET addressing.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//--
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "rfs_time.h"

//---------------------------------------------------------------------
//-- Time basis: the local wall clock is treated as if it were UTC
//-- (DSMRlogger32-compatible); daylight saving time is ignored. All
//-- formulas are fixed by the DSMRlogger32 baseline:
//--   HOUR   bucketId = unixSeconds / 3600
//--   DAY    bucketId = unixSeconds / 86400
//--   WEEK   bucketId = (unixSeconds + 259200) / 604800
//--          (boundaries at Monday 00:00; 259200 s shifts the Thursday
//--          1970-01-01 epoch onto the Monday 1969-12-29 week origin)
//--   MONTH  bucketId = (EEYY - 1) * 12 + MM
//-- The string form is "EEYYMMDDHHMMSS" with the century fixed to "20"
//-- (years 2000-2099).
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
                           uint64_t *outBucketId)
{
  if (outBucketId == NULL)
  {
    return false;
  }

  switch (granularity)
  {
    case RING_FILE_GRANULARITY_HOUR:
      *outBucketId = unixSeconds / RFS_TIME_SECONDS_PER_HOUR;
      return true;

    case RING_FILE_GRANULARITY_DAY:
      *outBucketId = unixSeconds / RFS_TIME_SECONDS_PER_DAY;
      return true;

    case RING_FILE_GRANULARITY_WEEK:
      //-- Monday-aligned: the 1970-01-01 (Thursday) epoch is shifted to
      //-- the Monday 1969-12-29 week origin.
      *outBucketId =
          (unixSeconds + RFS_TIME_WEEK_OFFSET_SECONDS) / RFS_TIME_WEEK_LENGTH_SECONDS;
      return true;

    case RING_FILE_GRANULARITY_MONTH:
    {
      int64_t year = 0;
      uint32_t month = 0, day = 0;
      rfsTimeCivilFromDays((int64_t)(unixSeconds / RFS_TIME_SECONDS_PER_DAY),
                           &year, &month, &day);
      //-- DSMRlogger32 month formula (before the modulo addressing).
      *outBucketId = (uint64_t)(year - 1) * 12u + (uint64_t)month;
      return true;
    }

    default:
      return false;
  }
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
                                 uint64_t *outBucketId)
{
  if (!rfsTimeUnixToBucketId(unixSeconds, granularity, outBucketId))
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
