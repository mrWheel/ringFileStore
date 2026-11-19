//--
//-- FileVersion: [1] 19-11-2026 - 13:25
//--
//-- ringFileStore - host test shim mapping ESP_LOGx to printf on stderr.
//-- Copyright (C) 2026 Willem Aandewiel (mrWheel)
//-- SPDX-License-Identifier: GPL-3.0-or-later
//--
#ifndef SHIM_ESP_LOG_H
#define SHIM_ESP_LOG_H

//-- Host test shim: ESP_LOGx map to printf on stderr.

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGD(tag, fmt, ...)

#ifdef __cplusplus
}
#endif

#endif
