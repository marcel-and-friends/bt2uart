#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum bt2uart_event_type_t {
    // Data received through uart
    BT2UART_EVENT_UART_RECV,

    // Data received through bluetooth spp
    BT2UART_EVENT_SPP_RECV,

    // Last spp write was successful
    BT2UART_EVENT_SPP_WRITE_SUCCEEDED,

    // Try writing again, either because of a previous write failure or because of congestion
    BT2UART_EVENT_SPP_WRITE_AGAIN,
    BT2UART_EVENT_SPP_CONGESTION_ENDED = BT2UART_EVENT_SPP_WRITE_AGAIN,
    BT2UART_EVENT_SPP_WRITE_FAILED = BT2UART_EVENT_SPP_WRITE_AGAIN,

    // Clear the spp buffer and reset the spp handle
    BT2UART_EVENT_SPP_RESET,
};

typedef struct {
    enum bt2uart_event_type_t type;

    union {
        struct {
            uint8_t* data;
            size_t len;
        } recv; // shared for both spp and uart receives

        struct {
            size_t num_bytes_written;
            bool congested;
        } write_succeeded;

        struct {
            uint32_t spp_handle;
        } reset;
    };
} bt2uart_event_t;

void bt2uart_event_send(bt2uart_event_t*);

esp_err_t bt2uart_event_loop_init();
