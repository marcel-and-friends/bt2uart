#pragma once

#include <esp_err.h>

#define UART_PORT UART_NUM_2
#define UART_BUFFER_SIZE 2048

esp_err_t bt2uart_uart_init();
