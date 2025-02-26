#include "bt.h"
#include <bt2uart/event.h>
#include <bt2uart/shared.h>
#include <bt2uart/util/err.h>
#include <bt2uart/util/log.h>
#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>
#include <esp_gatt_common_api.h>
#include <esp_gatt_defs.h>
#include <esp_gattc_api.h>
#include <esp_spp_api.h>
#include <nvs_flash.h>
#include <string.h>

#define PIN "sol"
#define DEVICE_NAME "BT2UART"
#define SERVER_NAME "BT2UART-SERVER"

static const char* bda2str(const uint8_t* bda) {
    static char output[18] = { 0 };

    const uint8_t* p = bda;
    sprintf(output, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return output;
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            LOGI("Authentication succeeded: %s bda:[%s]", param->auth_cmpl.device_name, bda2str(param->auth_cmpl.bda));
        } else {
            LOGE("Authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    default:
        LOGW("Unhandled gap event: %d", event);
        break;
    }
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            LOGI("ESP_SPP_INIT_EVT");
            LOG_ERR(esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, SERVER_NAME));
        } else {
            LOGE("ESP_SPP_INIT_EVT status:%d", param->init.status);
        }
        break;
    case ESP_SPP_START_EVT: {
        if (param->start.status == ESP_SPP_SUCCESS) {
            LOG_ERR(esp_bt_gap_set_device_name(DEVICE_NAME));
            LOG_ERR(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
        }
        LOGI("ESP_SPP_START_EVT handle:%lu sec_id:%d scn:%d", param->start.handle, param->start.sec_id, param->start.scn);
    } break;
    case ESP_SPP_SRV_OPEN_EVT:
        LOGI("ESP_SPP_SRV_OPEN_EVT status:%d handle:%lu rem_bda:[%s]", param->srv_open.status, param->srv_open.handle, bda2str(param->srv_open.rem_bda));
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        g_shared_ctx.spp_handle = param->srv_open.handle;
        break;
    case ESP_SPP_SRV_STOP_EVT:
        LOGI("ESP_SPP_SRV_STOP_EVT");
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        g_shared_ctx.spp_handle = 0;
        break;
    case ESP_SPP_OPEN_EVT:
        LOGI("ESP_SPP_OPEN_EVT");
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        g_shared_ctx.spp_handle = param->open.handle;
        break;
    case ESP_SPP_CLOSE_EVT:
        LOGI(
            "ESP_SPP_CLOSE_EVT status:%d handle:%lu close_by_remote:%d",
            param->close.status,
            param->close.handle,
            param->close.async);
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        g_shared_ctx.spp_handle = 0;
        break;
    case ESP_SPP_WRITE_EVT:
        LOGI("SPP_WRITE_EVT [%d]", param->write.len);
        if (param->write.status == ESP_SPP_SUCCESS) {
            bt2uart_event_t event = {
                .type = LUCAS_EVENT_SPP_WRITE_SUCCEEDED,
                .write_succeeded = {
                    .num_bytes_written = param->write.len,
                    .congested = param->write.cong,
                }
            };
            bt2uart_event_send(&event);
        } else {
            LOGE("write failed");
            if (!param->cong.cong) {
                // the write failed but not because of congestion, try again straight away.
                // otherwise, on congestion, we'll retry when `ESP_SPP_CONG_EVT` tells us that the congestion is over
                LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_WRITE_AGAIN);
            }
        }

        if (param->write.cong)
            LOGE("congested!");
        break;
    case ESP_SPP_DATA_IND_EVT: {
        LOGI("SPP_DATA_IND_EVT [%d]", param->data_ind.len);

        // SAFETY: the main event loop is responsible for freeing this
        uint8_t* data = malloc(param->data_ind.len);
        memcpy(data, param->data_ind.data, param->data_ind.len);

        bt2uart_event_t event = {
            .type = LUCAS_EVENT_SPP_RECV,
            .recv = {
                .data = data,
                .len = param->data_ind.len,
            }
        };
        bt2uart_event_send(&event);
    } break;
    case ESP_SPP_CONG_EVT:
        LOGI("ESP_SPP_CONG_EVT");
        if (param->cong.status == ESP_SPP_SUCCESS) {
            // congestion is over!
            if (!param->cong.cong)
                LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CONGESTION_ENDED);
        }
        break;
    case ESP_SPP_UNINIT_EVT:
        LOGI("ESP_SPP_UNINIT_EVT");
        break;
    default:
        LOGW("Unhandled spp event: %d", event);
        break;
    }
}

esp_err_t bt2uart_bt_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    TRY(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t bd_cfg = {
        .ssp_en = false
    };

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    TRY(esp_bt_controller_init(&bt_cfg));
    TRY(esp_bt_controller_enable(bt_cfg.mode));

    TRY(esp_bluedroid_init_with_cfg(&bd_cfg));
    TRY(esp_bluedroid_enable());

    // callbacks
    TRY(esp_bt_gap_register_callback(bt_gap_cb));
    TRY(esp_spp_register_callback(spp_cb));

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    TRY(esp_spp_enhanced_init(&spp_cfg));

    esp_bt_pin_code_t pin_code = PIN;
    TRY(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, strlen(PIN), pin_code));

    return ESP_OK;
}
