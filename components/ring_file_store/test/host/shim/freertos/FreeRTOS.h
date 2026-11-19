//--
//-- FileVersion: [1] 19-11-2026 - 13:25
//--
//-- ringFileStore - host test shim for the FreeRTOS primitives used by
//-- the component.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
#ifndef SHIM_FREERTOS_H
#define SHIM_FREERTOS_H

//-- Host test shim for the FreeRTOS primitives used by the component.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#ifdef __cplusplus
}
#endif

#endif
