/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include <driver/uart.h>
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include <freertos/queue.h>
#include "nvs.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fifo.h"

#define LOG_TAG "LUCAS_BT_SPP"
#define LOGI(...) ESP_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGW(...) ESP_LOGW(LOG_TAG, __VA_ARGS__)
#define LOGE(...) ESP_LOGE(LOG_TAG, __VA_ARGS__)
#define SPP_SERVER_NAME "LUCAS_SPP_SERVER"
#define EXAMPLE_DEVICE_NAME "LUCAS_BT_SPP"
#define UART_PORT UART_NUM_2

#define UART_BUFFER_SIZE 2048

typedef enum {
    // data received through uart
    LUCAS_UART_RECV,

    // data received through bluetooth spp
    LUCAS_SPP_RECV,

    // last spp write was successful
    LUCAS_SPP_WRITE_SUCCEEDED,

    // try writing again, either because of a previous write failure or because of congestion
    LUCAS_SPP_WRITE_AGAIN,

    LUCAS_SPP_UNCONGESTED = LUCAS_SPP_WRITE_AGAIN,
    LUCAS_SPP_WRITE_FAILED = LUCAS_SPP_WRITE_AGAIN,
} lucas_event_type_t;

typedef struct {
    lucas_event_type_t type;

    union {
        // shared for both spp and uart receives
        struct event_recv_param_t {
            uint8_t* data;
            size_t len;
        } recv;

        struct event_write_succeeded_param_t {
            size_t num_bytes_written;
            bool congested;
        } write_succeeded;
    };
} lucas_event_t;

struct lucas_shared_ctx_t {
    uint32_t spp_handle;

    QueueHandle_t event_queue;
} static g_shared_ctx;

typedef struct {
    fifo_t spp_fifo;

    bool spp_congested;
} lucas_event_loop_ctx_t;

typedef struct {
    QueueHandle_t event_queue;
} lucas_uart_event_loop_ctx_t;

static void lucas_uart_event_loop(void* octx) {
    lucas_uart_event_loop_ctx_t* ctx = octx;

    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/_images/esp32-devkitC-v4-pinout.png
    const int tx = 16;
    const int rx = 17;

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, tx, rx, -1, -1);
    uart_driver_install(UART_PORT, UART_BUFFER_SIZE, UART_BUFFER_SIZE, 10, &ctx->event_queue, 0);

    uint8_t* rx_data = malloc(UART_BUFFER_SIZE);

    uart_event_t event;
    while (true) {
        xQueueReceive(ctx->event_queue, &event, portMAX_DELAY);

        switch (event.type) {
        case UART_DATA:
            LOGI("UART [%zu]", event.size);
            if (uart_read_bytes(UART_PORT, rx_data, event.size, portMAX_DELAY) <= 0)
                break;

            // SAFETY: the main event loop is responsible for freeing this
            uint8_t* data = malloc(event.size);
            memcpy(data, rx_data, event.size);

            lucas_event_t bt_event = {
                .type = LUCAS_UART_RECV,
                .recv = {data, .len = event.size}
            };

            xQueueSend(g_shared_ctx.event_queue, &bt_event, portMAX_DELAY);

            break;
        case UART_FIFO_OVF:
            LOGI("hw fifo overflow");
            uart_flush_input(UART_PORT);
            xQueueReset(g_shared_ctx.event_queue);
            break;
        case UART_BUFFER_FULL:
            LOGI("ring buffer full");
            uart_flush_input(UART_PORT);
            xQueueReset(g_shared_ctx.event_queue);
            break;
        default:
            LOGI("uart event type: %d", event.type);
            break;
        }
    }

    free(rx_data);
}

static void lucas_event_loop(void* octx) {
    lucas_event_loop_ctx_t* ctx = octx;

    if (!fifo_init(&ctx->spp_fifo, UART_BUFFER_SIZE)) {
        LOGE("failed to allocate initial spp fifo");
        return;
    }

    lucas_event_t event;
    while (true) {
        xQueueReceive(g_shared_ctx.event_queue, &event, portMAX_DELAY);

        switch (event.type) {
        case LUCAS_UART_RECV:
            assert(event.recv.data && event.recv.len);

            // if the buffer is empty we don't have to wait for confirmation to write, since there's nothing to confirm
            bool write_without_waiting = ctx->spp_fifo.len == 0;

            fifo_push(&ctx->spp_fifo, event.recv.data, event.recv.len);
            LOGI("received uart data \"%.*s\" [%zu bytes - %zu total]", (int)event.recv.len, event.recv.data, event.recv.len, ctx->spp_fifo.len);

            if (!ctx->spp_congested && write_without_waiting)
                esp_spp_write(g_shared_ctx.spp_handle, ctx->spp_fifo.len, ctx->spp_fifo.data);

            free(event.recv.data);

            break;
        case LUCAS_SPP_RECV:
            assert(event.recv.data && event.recv.len && event.recv.len <= UART_BUFFER_SIZE);

            LOGI("received spp data [%zu bytes]", event.recv.len);
            uart_write_bytes(UART_PORT, event.recv.data, event.recv.len);

            free(event.recv.data);

            break;
        case LUCAS_SPP_WRITE_SUCCEEDED:
            assert(ctx->spp_fifo.len && !ctx->spp_congested);

            LOGI("sucessful spp write [%zu bytes - %zu left]", event.write_succeeded.num_bytes_written, ctx->spp_fifo.len - event.write_succeeded.num_bytes_written);

            // pop the bytes that were written
            fifo_pop(&ctx->spp_fifo, event.write_succeeded.num_bytes_written);

            ctx->spp_congested = event.write_succeeded.congested;
            if (!ctx->spp_congested && ctx->spp_fifo.len) {
                LOGI("continuing spp write [%zu bytes]", ctx->spp_fifo.len);
                // SAFETY: this function deep copies the buffer internally,
                //         there's no need to worry about modifications to the fifo being made before the transfer is finished
                //
                // @ref: https://github.com/espressif/esp-idf/blob/release/v5.2/components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c#L1442C13-L1442C33
                //       see the `btc_spp_arg_deep_copy` param
                esp_spp_write(g_shared_ctx.spp_handle, ctx->spp_fifo.len, ctx->spp_fifo.data);
            }

            break;
        case LUCAS_SPP_WRITE_AGAIN:
            assert(ctx->spp_fifo.len);

            // receiving this event definitely means that there's no congestion,
            // either because a congestion has ended (see `ESP_SPP_WRITE_EVT` handling),
            // or because the last write failed but not because of congestion.
            // to account for the first case we set the flag to false
            ctx->spp_congested = false;

            LOGW("retrying to write spp data [%zu bytes]", ctx->spp_fifo.len);
            // SAFETY: this function deep copies the buffer internally,
            //         there's no need to worry about modifications to the fifo being made before the transfer is finished
            //
            // @ref: https://github.com/espressif/esp-idf/blob/release/v5.2/components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c#L1442C13-L1442C33
            //       see the `btc_spp_arg_deep_copy` param
            esp_spp_write(g_shared_ctx.spp_handle, ctx->spp_fifo.len, ctx->spp_fifo.data);

            break;
        }
    }
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
    bool write_succeeded = false;
    switch (event) {
    case ESP_SPP_WRITE_EVT:
        LOGI("WRITE_EVT [%d]", param->write.len);

        if (param->write.cong)
            LOGE("congested!");

        write_succeeded = param->write.status == ESP_SPP_SUCCESS;
        if (write_succeeded) {
            lucas_event_t success_event = {
                .type = LUCAS_SPP_WRITE_SUCCEEDED,
                .write_succeeded = {.num_bytes_written = param->write.len,
                                    .congested = param->write.cong}
            };
            xQueueSend(g_shared_ctx.event_queue, &success_event, portMAX_DELAY);
        } else {
            LOGE("write failed");
            if (!param->cong.cong) {
                // the write failed but not because of congestion, try again straight away.
                // otherwise, on congestion, we'll retry when `ESP_SPP_CONG_EVT` tells us that the congestion is over
                lucas_event_t event = { .type = LUCAS_SPP_WRITE_FAILED };
                xQueueSend(g_shared_ctx.event_queue, &event, portMAX_DELAY);
            }
        }
        break;
    case ESP_SPP_CONG_EVT:
        LOGI("ESP_SPP_CONG_EVT");
        // congestion is over!
        if (!param->cong.cong) {
            lucas_event_t event = { .type = LUCAS_SPP_UNCONGESTED };
            xQueueSend(g_shared_ctx.event_queue, &event, portMAX_DELAY);
        }

        break;
    case ESP_SPP_DATA_IND_EVT: {
        // SAFETY: the main event loop is responsible for freeing this
        uint8_t* data = malloc(param->data_ind.len);
        memcpy(data, param->data_ind.data, param->data_ind.len);

        lucas_event_t event = {
            .type = LUCAS_SPP_RECV,
            .recv = {.data = data,
                     .len = param->data_ind.len}
        };
        xQueueSend(g_shared_ctx.event_queue, &event, portMAX_DELAY);
    } break;
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            LOGI("ESP_SPP_INIT_EVT");
            esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVER_NAME);
        } else {
            LOGE("ESP_SPP_INIT_EVT status:%d", param->init.status);
        }
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        LOGI("ESP_SPP_DISCOVERY_COMP_EVT");
        break;
    case ESP_SPP_OPEN_EVT:
        g_shared_ctx.spp_handle = param->open.handle;
        LOGI("ESP_SPP_OPEN_EVT");
        break;
    case ESP_SPP_CLOSE_EVT: {
        g_shared_ctx.spp_handle = 0;
        LOGI(
            "ESP_SPP_CLOSE_EVT status:%d handle:%" PRIu32
            " close_by_remote:%d",
            param->close.status,
            param->close.handle,
            param->close.async);
    } break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            LOGI("ESP_SPP_START_EVT handle:%" PRIu32 " sec_id:%d scn:%d", param->start.handle, param->start.sec_id, param->start.scn);
            esp_bt_dev_set_device_name(EXAMPLE_DEVICE_NAME);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        } else {
            LOGE("ESP_SPP_START_EVT status:%d", param->start.status);
        }
        break;
    case ESP_SPP_CL_INIT_EVT:
        LOGI("ESP_SPP_CL_INIT_EVT");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        g_shared_ctx.spp_handle = param->srv_open.handle;
        break;
    case ESP_SPP_SRV_STOP_EVT:
        g_shared_ctx.spp_handle = 0;
        LOGI("ESP_SPP_SRV_STOP_EVT");
        break;
    case ESP_SPP_UNINIT_EVT:
        g_shared_ctx.spp_handle = 0;
        LOGI("ESP_SPP_UNINIT_EVT");
        break;
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            LOGI("authentication success: %s", param->auth_cmpl.device_name);
        } else {
            LOGE("authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        LOGI("ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            LOGI("Input pin code: F0F0 F0F0 F0F0 F0F0");
            esp_bt_pin_code_t pin_code = { 0xF0 };
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            LOGI("Input pin code: F0F0");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = 'F';
            pin_code[1] = '0';
            pin_code[2] = 'F';
            pin_code[3] = '0';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        LOGI("ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %" PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        LOGI("ESP_BT_GAP_KEY_NOTIF_EVT passkey:%" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        LOGI("ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    case ESP_BT_GAP_MODE_CHG_EVT:
        LOGI("ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
        break;

    default: {
        LOGI("event: %d", event);
        break;
    }
    }
    return;
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        LOGE("%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        LOGE("%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_init()) != ESP_OK) {
        LOGE("%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        LOGE("%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    memset(&g_shared_ctx, 0, sizeof(g_shared_ctx));
    g_shared_ctx.event_queue = xQueueCreate(20, sizeof(lucas_event_t));

    static lucas_uart_event_loop_ctx_t uart_ctx = { 0 };
    xTaskCreate(lucas_uart_event_loop, "UART", 4096, &uart_ctx, 20, NULL);

    static lucas_event_loop_ctx_t main_ctx = { 0 };
    xTaskCreate(lucas_event_loop, "MAIN", 4096, &main_ctx, 12, NULL);

    if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK) {
        LOGE("%s gap register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_spp_register_callback(esp_spp_cb)) != ESP_OK) {
        LOGE("%s spp register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0, /* Only used for ESP_SPP_MODE_VFS mode */
    };

    if ((ret = esp_spp_enhanced_init(&bt_spp_cfg)) != ESP_OK) {
        LOGE("%s spp init failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);
}
