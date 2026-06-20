#include "event.h"
#include <bt2uart/uart.h>
#include <bt2uart/util/err.h>
#include <bt2uart/util/fifo.h>
#include <bt2uart/util/log.h>
#include <driver/uart.h>
#include <esp_spp_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct event_loop_ctx_t {
    bt2uart_fifo_t spp_fifo_buffer;
    bt2uart_fifo_t uart_tx_fifo;
    uint32_t spp_handle;
    bool spp_congested;
};

#define QUEUE_LENGTH 64
static QueueHandle_t s_event_queue;
static StaticQueue_t s_event_queue_data;
static uint8_t s_event_queue_storage[QUEUE_LENGTH * sizeof(bt2uart_event_t)];

#define STACK_SIZE 4096
static StaticTask_t s_task_data;
static StackType_t s_task_stack[STACK_SIZE];

static void write_fifo_to_spp(bt2uart_fifo_t* fifo, uint32_t spp_handle) {
    // SAFETY: this function deep copies the buffer internally,
    //         there's no need to worry about modifications to the fifo being made before the transfer is finished
    //         https://github.com/espressif/esp-idf/blob/release/v5.2/components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c#L1442C13-L1442C33
    //         see the `btc_spp_arg_deep_copy` param
    LOG_ERR(esp_spp_write(spp_handle, fifo->len, fifo->data));
}

// Drains the SPP->UART fifo into the UART without ever blocking.
// `uart_tx_chars` only writes what currently fits in the HW FIFO and returns
// immediately, so the MAIN task never stalls on a slow STM32. A stall here
// would back up the event queue and, because `bt2uart_event_send` blocks with
// portMAX_DELAY, would freeze the bluedroid task — which is what was causing the
// 5s BT link supervision timeouts (HCI_ERR_CONNECTION_TOUT). Bytes that don't
// fit stay buffered in the (growing) fifo and are flushed on the next wake-up.
static void drain_uart_tx(struct event_loop_ctx_t* ctx) {
    while (ctx->uart_tx_fifo.len) {
        int written = uart_tx_chars(UART_PORT, (const char*)ctx->uart_tx_fifo.data, ctx->uart_tx_fifo.len);
        if (written <= 0)
            break;
        bt2uart_fifo_pop(&ctx->uart_tx_fifo, written);
    }
}

static void event_loop(void* octx) {
    struct event_loop_ctx_t* ctx = octx;

    bt2uart_event_t event;
    while (true) {
        // when there are still UART bytes pending, wake up periodically to flush
        // them; otherwise block until the next event.
        TickType_t timeout = ctx->uart_tx_fifo.len ? pdMS_TO_TICKS(2) : portMAX_DELAY;
        if (xQueueReceive(s_event_queue, &event, timeout) == pdFALSE) {
            drain_uart_tx(ctx);
            continue;
        }

        switch (event.type) {
        case BT2UART_EVENT_UART_RECV:
            if (!ctx->spp_handle) {
                LOGW("UART_RECV - no bt connection");
                free(event.recv.data);
                break;
            }

            assert(event.recv.data && event.recv.len);

            LOGI("UART_RECV -\n%.*s\n(len = %zu | fifo = %zu)", (int)event.recv.len, event.recv.data, event.recv.len, ctx->spp_fifo_buffer.len);

            // if there's no data currently buffered begin writing straight away
            // NOTE: this has to be evaluated before `bt2uart_fifo_push` so that the len is not affected by the push.
            bool write_straight_away = ctx->spp_fifo_buffer.len == 0 && !ctx->spp_congested;
            bt2uart_fifo_push(&ctx->spp_fifo_buffer, event.recv.data, event.recv.len);
            if (write_straight_away)
                write_fifo_to_spp(&ctx->spp_fifo_buffer, ctx->spp_handle);

            free(event.recv.data);
            break;
        case BT2UART_EVENT_SPP_RECV:
            assert(event.recv.data && event.recv.len && event.recv.len <= UART_BUFFER_SIZE);

            LOGI("SPP_RECV -\n%.*s\n(len = %zu)", event.recv.len, event.recv.data, event.recv.len);

            // Buffer the bytes and flush them non-blocking. We never drop and
            // never fragment (the fifo preserves byte order); we just refuse to
            // block the task, unlike the old uart_write_bytes call.
            bt2uart_fifo_push(&ctx->uart_tx_fifo, event.recv.data, event.recv.len);
            free(event.recv.data);
            drain_uart_tx(ctx);
            break;
        case BT2UART_EVENT_SPP_WRITE_SUCCEEDED:
            if (ctx->spp_fifo_buffer.len == 0) {
                LOGE("SPP_WRITE_SUCCEEDED - fifo is empty");
                break;
            }

            assert(event.write_succeeded.num_bytes_written <= ctx->spp_fifo_buffer.len && !ctx->spp_congested);

            LOGI("SPP_WRITE_SUCCEEDED -\n%.*s\n(written = %zu | rem = %zu)", event.write_succeeded.num_bytes_written, ctx->spp_fifo_buffer.data, event.write_succeeded.num_bytes_written, ctx->spp_fifo_buffer.len - event.write_succeeded.num_bytes_written);

            // pop the bytes that were written
            bt2uart_fifo_pop(&ctx->spp_fifo_buffer, event.write_succeeded.num_bytes_written);

            ctx->spp_congested = event.write_succeeded.congested;
            if (!ctx->spp_congested && ctx->spp_fifo_buffer.len)
                write_fifo_to_spp(&ctx->spp_fifo_buffer, ctx->spp_handle);

            break;
        case BT2UART_EVENT_SPP_WRITE_AGAIN:
            if (ctx->spp_fifo_buffer.len == 0) {
                LOGE("SPP_WRITE_AGAIN - fifo is empty");
                break;
            }

            // receiving this event definitely means that there's no congestion,
            // either because a congestion has ended (see `ESP_SPP_WRITE_EVT` handling),
            // or because the last write failed but *not* because of congestion.
            ctx->spp_congested = false;

            LOGW("SPP_WRITE_AGAIN - fifo = %zu", ctx->spp_fifo_buffer.len);
            write_fifo_to_spp(&ctx->spp_fifo_buffer, ctx->spp_handle);

            break;
        case BT2UART_EVENT_SPP_RESET:
            LOGW("SPP_RESET - fifo = %zu | handle = %u", ctx->spp_fifo_buffer.len, event.reset.spp_handle);

            bt2uart_fifo_clear(&ctx->spp_fifo_buffer);
            // also drop any half-sent command bound for the STM32 so a dropped
            // connection never delivers a truncated line to the machine.
            bt2uart_fifo_clear(&ctx->uart_tx_fifo);
            ctx->spp_handle = event.reset.spp_handle;
            ctx->spp_congested = false;

            break;
        }
    }
}

void bt2uart_event_send(bt2uart_event_t* event) {
    xQueueSend(s_event_queue, event, portMAX_DELAY);
}

esp_err_t bt2uart_event_loop_init() {
    // NOTE: the static lifetime is very important,
    //       this object has to live as long as the task itself, which is forever
    static struct event_loop_ctx_t ctx = { 0 };

    s_event_queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(bt2uart_event_t), s_event_queue_storage, &s_event_queue_data);
    TRY(bt2uart_fifo_init(&ctx.spp_fifo_buffer, UART_BUFFER_SIZE));
    TRY(bt2uart_fifo_init(&ctx.uart_tx_fifo, UART_BUFFER_SIZE));
    xTaskCreateStatic(event_loop, "MAIN", STACK_SIZE, &ctx, 16, s_task_stack, &s_task_data);

    return ESP_OK;
}
