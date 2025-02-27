#pragma once

#include "esp_log.h"

#define BT2UART_LOG_TAG "BT2UART"
#define LOGI(...) ESP_LOGI(BT2UART_LOG_TAG, __VA_ARGS__)
#define LOGW(...) ESP_LOGW(BT2UART_LOG_TAG, __VA_ARGS__)
#define LOGE(...) ESP_LOGE(BT2UART_LOG_TAG, __VA_ARGS__)
