#pragma once

#include "esp_log.h"

#define LUCAS_LOG_TAG "LUCAS"
#define LUCAS_LOGI(...) ESP_LOGI(LUCAS_LOG_TAG, __VA_ARGS__)
#define LUCAS_LOGW(...) ESP_LOGW(LUCAS_LOG_TAG, __VA_ARGS__)
#define LUCAS_LOGE(...) ESP_LOGE(LUCAS_LOG_TAG, __VA_ARGS__)
