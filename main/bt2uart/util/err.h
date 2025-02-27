#pragma once

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#define BT2UART_FILENAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define TRY(expr)                                                                                                                               \
    do {                                                                                                                                        \
        esp_err_t err = (expr);                                                                                                                 \
        if (err != ESP_OK) {                                                                                                                    \
            ESP_LOGE("BT2UART_ERROR", "ESP32 FAIL @%s:%d - `%s` -> \"%s\" [%d]", BT2UART_FILENAME, __LINE__, #expr, esp_err_to_name(err), err); \
            return err;                                                                                                                         \
        }                                                                                                                                       \
    } while (0)

#define LOG_ERR(expr)                                                                                                                           \
    do {                                                                                                                                        \
        esp_err_t err = (expr);                                                                                                                 \
        if (err != ESP_OK) {                                                                                                                    \
            ESP_LOGE("BT2UART_ERROR", "ESP32 FAIL @%s:%d - `%s` -> \"%s\" [%d]", BT2UART_FILENAME, __LINE__, #expr, esp_err_to_name(err), err); \
        }                                                                                                                                       \
    } while (0)
