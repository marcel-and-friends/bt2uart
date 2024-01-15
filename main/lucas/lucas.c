#include "lucas.h"
#include <lucas/util/err.h>
#include <lucas/event.h>
#include <lucas/uart.h>
#include <lucas/bt.h>

esp_err_t lucas_init() {
    TRY(lucas_uart_init());
    TRY(lucas_event_loop_init());
    TRY(lucas_bt_init());

    return ESP_OK;
}
