#include "uart.h"
#include <bt2uart/event.h>
#include <bt2uart/util/err.h>
#include <bt2uart/util/log.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

struct uart_event_loop_ctx_t {
    QueueHandle_t event_queue;
    uint8_t rx_buffer[UART_BUFFER_SIZE];
};

#define STACK_SIZE 4096
static StaticTask_t s_task_data;
static StackType_t s_task_stack[STACK_SIZE];

static void uart_event_loop(void* octx) {
    struct uart_event_loop_ctx_t* ctx = octx;

    uart_event_t event;
    while (true) {
        xQueueReceive(ctx->event_queue, &event, portMAX_DELAY);

        switch (event.type) {
        case UART_DATA:
            LOGI("UART [%zu]", event.size);
            if (uart_read_bytes(UART_PORT, ctx->rx_buffer, event.size, portMAX_DELAY) <= 0)
                break;

            // SAFETY: the main event loop is responsible for freeing this
            uint8_t* data = malloc(event.size);
            assert(data);

            memcpy(data, ctx->rx_buffer, event.size);

            bt2uart_event_send(&(bt2uart_event_t) {
                .type = BT2UART_EVENT_UART_RECV,
                .recv = { data, event.size },
            });
            break;
        case UART_FIFO_OVF:
            LOGE("UART_FIFO_OVF");
            LOG_ERR(uart_flush_input(UART_PORT));
            break;
        case UART_BUFFER_FULL:
            LOGE("UART_BUFFER_FULL");
            LOG_ERR(uart_flush_input(UART_PORT));
            break;
        default:
            LOGW("unhandled uart event: %d", event.type);
            break;
        }
    }
}

esp_err_t bt2uart_uart_init() {
    // NOTE: the static lifetime is very important,
    //       this object has to live as long as the task itself, which is forever
    static struct uart_event_loop_ctx_t ctx = { 0 };

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    TRY(uart_param_config(UART_PORT, &uart_config));
    TRY(uart_set_pin(UART_PORT, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    TRY(uart_driver_install(UART_PORT, UART_BUFFER_SIZE, UART_BUFFER_SIZE, 20, &ctx.event_queue, 0));

    xTaskCreateStatic(uart_event_loop, "UART", STACK_SIZE, &ctx, 20, s_task_stack, &s_task_data);

    return ESP_OK;
}
