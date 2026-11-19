//--
//-- FileVersion: [1] 19-11-2026 - 13:25
//--
//-- ringFileStore - host test shim for the ESP-IDF esp_err.h API.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
#ifndef SHIM_ESP_ERR_H
#define SHIM_ESP_ERR_H

//-- Host test shim for the ESP-IDF esp_err.h API.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x103
#define ESP_ERR_INVALID_STATE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107

const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif

#endif
