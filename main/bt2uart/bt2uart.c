#include "bt2uart.h"
#include <bt2uart/bt.h>
#include <bt2uart/event.h>
#include <bt2uart/uart.h>
#include <bt2uart/util/err.h>

esp_err_t bt2uart_init() {
    TRY(bt2uart_uart_init());
    TRY(bt2uart_event_loop_init());
    TRY(bt2uart_bt_init());

    return ESP_OK;
}
