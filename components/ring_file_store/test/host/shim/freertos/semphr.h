//--
//-- FileVersion: [1] 19-11-2026 - 13:25
//--
//-- ringFileStore - host test shim: FreeRTOS mutexes on pthread mutexes.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
#ifndef SHIM_SEMPHR_H
#define SHIM_SEMPHR_H

//-- Host test shim: FreeRTOS mutexes on top of pthread mutexes.

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#ifdef __cplusplus
}
#endif

#endif
