#pragma once

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define LUCAS_FILENAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LUCAS_ESP_TRY(expr)                                                                                                                 \
    do {                                                                                                                                    \
        esp_err_t err = (expr);                                                                                                             \
        if (err != ESP_OK) {                                                                                                                \
            ESP_LOGE("LUCAS_ERROR", "ESP32 FAIL @%s:%d - `%s` -> \"%s\" [%d]", LUCAS_FILENAME, __LINE__, #expr, esp_err_to_name(err), err); \
            return err;                                                                                                                     \
        }                                                                                                                                   \
    } while (0)

#define LUCAS_FREERTOS_TRY(expr)                                                                                   \
    do {                                                                                                           \
        BaseType_t err = (expr);                                                                                   \
        if (err != pdPASS) {                                                                                       \
            ESP_LOGE("LUCAS_ERROR", "FREERTOS ERROR @%s:%d - `%s` -> [%d]", LUCAS_FILENAME, __LINE__, #expr, err); \
            return ESP_FAIL;                                                                                       \
        }                                                                                                          \
    } while (0)
