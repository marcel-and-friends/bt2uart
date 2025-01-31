#include "event.h"
#include <bt2uart/shared.h>
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
};

static QueueHandle_t s_event_queue = NULL;

static void write_fifo_to_spp(bt2uart_fifo_t* fifo, uint32_t spp_handle) {
    // SAFETY: this function deep copies the buffer internally,
    //         there's no need to worry about modifications to the fifo being made before the transfer is finished
    //         https://github.com/espressif/esp-idf/blob/release/v5.2/components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c#L1442C13-L1442C33
    //         see the `btc_spp_arg_deep_copy` param
    esp_spp_write(spp_handle, fifo->len, fifo->data);
}

static void event_loop(void* octx) {
    struct event_loop_ctx_t* ctx = octx;

    bool spp_congested = false;
    bt2uart_event_t event;
    while (true) {
        xQueueReceive(s_event_queue, &event, portMAX_DELAY);

        switch (event.type) {
        case LUCAS_EVENT_UART_RECV:
            if (!g_shared_ctx.spp_handle) {
                free(event.recv.data);
                break;
            }

            assert(event.recv.data && event.recv.len);

            LOGI("received uart data \"%.*s\" [%zu bytes - %zu total]", (int)event.recv.len, event.recv.data, event.recv.len, ctx->spp_fifo_buffer.len);

            // if there's no data currently buffered begin writing straight away
            // NOTE: this has to be evaluated before `bt2uart_fifo_push` so that the len is not affected by the push.
            bool write_straight_away = ctx->spp_fifo_buffer.len == 0 && !spp_congested;
            bt2uart_fifo_push(&ctx->spp_fifo_buffer, event.recv.data, event.recv.len);
            if (write_straight_away)
                write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);

            free(event.recv.data);
            break;
        case LUCAS_EVENT_SPP_RECV:
            assert(event.recv.data && event.recv.len && event.recv.len <= LUCAS_UART_BUFFER_SIZE);

            LOGI("received spp data [%zu bytes]", event.recv.len);
            uart_write_bytes(LUCAS_UART_PORT, event.recv.data, event.recv.len);

            free(event.recv.data);
            break;
        case LUCAS_EVENT_SPP_WRITE_SUCCEEDED:
            assert(ctx->spp_fifo_buffer.len && event.write_succeeded.num_bytes_written <= ctx->spp_fifo_buffer.len && !spp_congested);

            LOGI("sucessful spp write [%zu bytes - %zu left]", event.write_succeeded.num_bytes_written, ctx->spp_fifo_buffer.len - event.write_succeeded.num_bytes_written);

            // pop the bytes that were written
            bt2uart_fifo_pop(&ctx->spp_fifo_buffer, event.write_succeeded.num_bytes_written);

            spp_congested = event.write_succeeded.congested;
            if (!spp_congested && ctx->spp_fifo_buffer.len) {
                LOGI("continuing spp write [%zu bytes]", ctx->spp_fifo_buffer.len);
                write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);
            }

            break;
        case LUCAS_EVENT_SPP_WRITE_AGAIN:
            assert(ctx->spp_fifo_buffer.len);

            // receiving this event definitely means that there's no congestion,
            // either because a congestion has ended (see `ESP_SPP_WRITE_EVT` handling),
            // or because the last write failed but *not* because of congestion.
            spp_congested = false;

            LOGW("retrying to write spp data [%zu bytes]", ctx->spp_fifo_buffer.len);
            write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);

            break;
        case LUCAS_EVENT_SPP_CLEAR_BUFFER:
            LOGW("cleared spp buffer [%zu bytes]", ctx->spp_fifo_buffer.len);
            bt2uart_fifo_clear(&ctx->spp_fifo_buffer);

            break;
        }
    }
}

void bt2uart_event_send(bt2uart_event_t* event) {
    assert(xQueueSend(s_event_queue, event, portMAX_DELAY) == pdPASS);
}

esp_err_t bt2uart_event_loop_init() {
    // NOTE: the static lifetime is very important,
    //       this object has to live as long as the task itself, which is forever
    static struct event_loop_ctx_t ctx = { 0 };

    if (!bt2uart_fifo_init(&ctx.spp_fifo_buffer, LUCAS_UART_BUFFER_SIZE))
        return ESP_ERR_NO_MEM;

    s_event_queue = xQueueCreate(20, sizeof(bt2uart_event_t));
    if (s_event_queue == NULL)
        return ESP_ERR_NO_MEM;

    RTOS_TRY(xTaskCreate(event_loop, "MAIN", 4096, &ctx, 16, NULL));

    return ESP_OK;
}
