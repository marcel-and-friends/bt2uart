#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#define LUCAS_EVENT_SEND(t)                  \
    do {                                     \
        lucas_event_t event = { .type = t }; \
        lucas_event_send(&event);            \
    } while (0)

enum lucas_event_type_t {
    // data received through uart
    LUCAS_EVENT_UART_RECV,

    // data received through bluetooth spp
    LUCAS_EVENT_SPP_RECV,

    // last spp write was successful
    LUCAS_EVENT_SPP_WRITE_SUCCEEDED,

    // try writing again, either because of a previous write failure or because of congestion
    LUCAS_EVENT_SPP_WRITE_AGAIN,
    LUCAS_EVENT_SPP_CONGESTION_ENDED = LUCAS_EVENT_SPP_WRITE_AGAIN,
    LUCAS_EVENT_SPP_WRITE_FAILED = LUCAS_EVENT_SPP_WRITE_AGAIN,

    // clear the spp buffer
    LUCAS_EVENT_SPP_CLEAR_BUFFER,
};

typedef struct {
    enum lucas_event_type_t type;

    union {
        struct {
            uint8_t* data;
            size_t len;
        } recv; // shared for both spp and uart receives

        struct {
            size_t num_bytes_written;
            bool congested;
        } write_succeeded;
    };
} lucas_event_t;

void lucas_event_send(lucas_event_t*);

esp_err_t lucas_event_loop_init();
