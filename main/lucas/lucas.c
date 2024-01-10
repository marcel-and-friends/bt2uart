#include "lucas.h"
#include <lucas/util/err.h>
#include <lucas/event.h>
#include <lucas/uart.h>
#include <lucas/bt.h>

esp_err_t lucas_init() {
    LUCAS_ESP_TRY(lucas_uart_init());
    LUCAS_ESP_TRY(lucas_event_loop_init());
    LUCAS_ESP_TRY(lucas_bt_init());

    return ESP_OK;
}
