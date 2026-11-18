//--
//-- FileVersion: [2] 18-11-2026 - 19:40
//--
//-- ringFileStore - POSIX shim for the host tests (esp_err, esp_log,
//-- FreeRTOS mutex).
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//--
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ring_file_store.h"

#include <pthread.h>
#include <stdlib.h>

const char *esp_err_to_name(esp_err_t code)
{
  switch (code)
  {
    case ESP_OK:
      return "ESP_OK";
    case ESP_FAIL:
      return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
      return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
      return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_SIZE:
      return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_INVALID_STATE:
      return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:
      return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
      return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
      return "ESP_ERR_TIMEOUT";
    case RING_FILE_STORE_ERR_RING_EMPTY:
      return "RING_FILE_STORE_ERR_RING_EMPTY";
    case RING_FILE_STORE_ERR_KEY_NOT_FOUND:
      return "RING_FILE_STORE_ERR_KEY_NOT_FOUND";
    case RING_FILE_STORE_ERR_DUPLICATE_KEY:
      return "RING_FILE_STORE_ERR_DUPLICATE_KEY";
    case RING_FILE_STORE_ERR_ITER_END:
      return "RING_FILE_STORE_ERR_ITER_END";
    case RING_FILE_STORE_ERR_ITER_INVALIDATED:
      return "RING_FILE_STORE_ERR_ITER_INVALIDATED";
    case RING_FILE_STORE_ERR_SCHEMA_MISMATCH:
      return "RING_FILE_STORE_ERR_SCHEMA_MISMATCH";
    case RING_FILE_STORE_ERR_FORMAT:
      return "RING_FILE_STORE_ERR_FORMAT";
    case RING_FILE_STORE_ERR_CORRUPT:
      return "RING_FILE_STORE_ERR_CORRUPT";
    case RING_FILE_STORE_ERR_READONLY:
      return "RING_FILE_STORE_ERR_READONLY";
    case RING_FILE_STORE_ERR_IO:
      return "RING_FILE_STORE_ERR_IO";
    case RING_FILE_STORE_ERR_RECOVERY_REQUIRED:
      return "RING_FILE_STORE_ERR_RECOVERY_REQUIRED";
    case RING_FILE_STORE_ERR_IO_FAULTED:
      return "RING_FILE_STORE_ERR_IO_FAULTED";
    case RING_FILE_STORE_ERR_STORAGE_FULL:
      return "RING_FILE_STORE_ERR_STORAGE_FULL";
    case RING_FILE_STORE_ERR_ALREADY_EXISTS:
      return "RING_FILE_STORE_ERR_ALREADY_EXISTS";
    case RING_FILE_STORE_ERR_BUSY:
      return "RING_FILE_STORE_ERR_BUSY";
    default:
      return "UNKNOWN";
  }
}

typedef struct
{
  pthread_mutex_t mutex;
} shim_mutex_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
  shim_mutex_t *m = (shim_mutex_t *)calloc(1, sizeof(*m));

  if (m == NULL)
  {
    return NULL;
  }

  if (pthread_mutex_init(&m->mutex, NULL) != 0)
  {
    free(m);
    return NULL;
  }

  return (SemaphoreHandle_t)m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
  (void)ticks;
  shim_mutex_t *m = (shim_mutex_t *)semaphore;

  if (m == NULL)
  {
    return pdFALSE;
  }

  return (pthread_mutex_lock(&m->mutex) == 0) ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
  shim_mutex_t *m = (shim_mutex_t *)semaphore;

  if (m == NULL)
  {
    return pdFALSE;
  }

  return (pthread_mutex_unlock(&m->mutex) == 0) ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
  shim_mutex_t *m = (shim_mutex_t *)semaphore;

  if (m == NULL)
  {
    return;
  }

  pthread_mutex_destroy(&m->mutex);
  free(m);
}
