#include "bt.h"
#include <string.h>
#include <nvs_flash.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_spp_api.h>
#include <lucas/shared.h>
#include <lucas/event.h>
#include <lucas/util/log.h>
#include <lucas/util/err.h>

#define PIN "cafe"
#define DEVICE_NAME "LUCAS_BT"
#define SERVER_NAME "LUCAS_BT_SERVER"

static char* bda2str(uint8_t* bda, char* str, size_t size) {
    if (bda == NULL || str == NULL || size < 18)
        return NULL;

    uint8_t* p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
    char bda_str[18] = { 0 };
    switch (event) {
    case ESP_SPP_WRITE_EVT:
        LUCAS_LOGI("SPP_WRITE_EVT [%d]", param->write.len);
        if (param->write.status == ESP_SPP_SUCCESS) {
            lucas_event_t event = {
                .type = LUCAS_EVENT_SPP_WRITE_SUCCEEDED,
                .write_succeeded = {.num_bytes_written = param->write.len,
                                    .congested = param->write.cong}
            };
            lucas_event_send(&event);
        } else {
            LUCAS_LOGE("write failed");
            if (!param->cong.cong) {
                // the write failed but not because of congestion, try again straight away.
                // otherwise, on congestion, we'll retry when `ESP_SPP_CONG_EVT` tells us that the congestion is over
                LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_WRITE_AGAIN);
            }
        }

        g_shared_ctx.spp_handle = param->write.handle;
        if (param->write.cong)
            LUCAS_LOGE("congested!");
        break;
    case ESP_SPP_DATA_IND_EVT: {
        LUCAS_LOGI("SPP_DATA_IND_EVT [%d]", param->data_ind.len);

        // SAFETY: the main event loop is responsible for freeing this
        uint8_t* data = malloc(param->data_ind.len);
        memcpy(data, param->data_ind.data, param->data_ind.len);

        lucas_event_t event = {
            .type = LUCAS_EVENT_SPP_RECV,
            .recv = {.data = data,
                     .len = param->data_ind.len}
        };
        lucas_event_send(&event);

        g_shared_ctx.spp_handle = param->data_ind.handle;
    } break;
    case ESP_SPP_CONG_EVT:
        LUCAS_LOGI("ESP_SPP_CONG_EVT");
        // congestion is over!
        if (!param->cong.cong)
            LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CONGESTION_ENDED);

        g_shared_ctx.spp_handle = param->cong.handle;
        break;
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            LUCAS_LOGI("ESP_SPP_INIT_EVT");
            ESP_ERROR_CHECK(esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, SERVER_NAME));
            LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        } else {
            LUCAS_LOGE("ESP_SPP_INIT_EVT status:%d", param->init.status);
        }
        break;
    case ESP_SPP_START_EVT:
        LUCAS_LOGI("ESP_SPP_START_EVT handle:%lu sec_id:%d scn:%d", param->start.handle, param->start.sec_id, param->start.scn);
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        LUCAS_LOGI("ESP_SPP_SRV_OPEN_EVT status:%d handle:%lu rem_bda:[%s]", param->srv_open.status, param->srv_open.handle, bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        break;
    case ESP_SPP_OPEN_EVT:
        LUCAS_LOGI("ESP_SPP_OPEN_EVT");
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        g_shared_ctx.spp_handle = param->open.handle;
        break;
    case ESP_SPP_CLOSE_EVT:
        LUCAS_LOGI(
            "ESP_SPP_CLOSE_EVT status:%d handle:%lu close_by_remote:%d",
            param->close.status,
            param->close.handle,
            param->close.async);
        g_shared_ctx.spp_handle = 0;
        break;
    case ESP_SPP_SRV_STOP_EVT:
        LUCAS_LOGI("ESP_SPP_SRV_STOP_EVT");
        g_shared_ctx.spp_handle = 0;
        break;
    case ESP_SPP_UNINIT_EVT:
        LUCAS_LOGI("ESP_SPP_UNINIT_EVT");
        g_shared_ctx.spp_handle = 0;
        break;
    default:
        LUCAS_LOGW("unhandled spp event: %d", event);
        break;
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    char bda_str[18] = { 0 };
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            LUCAS_LOGI("authentication succeeded: %s bda:[%s]", param->auth_cmpl.device_name, bda2str(param->auth_cmpl.bda, bda_str, sizeof(bda_str)));
        } else {
            LUCAS_LOGE("authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        LUCAS_LOGI("ESP_BT_GAP_MODE_CHG_EVT mode:%d bda:[%s]", param->mode_chg.mode, bda2str(param->mode_chg.bda, bda_str, sizeof(bda_str)));
        break;
    default:
        LUCAS_LOGW("unhandled gap event: %d", event);
        break;
    }
}

esp_err_t lucas_bt_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    LUCAS_ESP_TRY(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    // bt controller
    LUCAS_ESP_TRY(esp_bt_controller_init(&bt_cfg));
    LUCAS_ESP_TRY(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    // bluedroid
    LUCAS_ESP_TRY(esp_bluedroid_init());
    LUCAS_ESP_TRY(esp_bluedroid_enable());

    // device
    LUCAS_ESP_TRY(esp_bt_dev_set_device_name(DEVICE_NAME));

    // gap
    esp_bt_pin_code_t pin_code = PIN;
    LUCAS_ESP_TRY(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, strlen(PIN), pin_code));
    LUCAS_ESP_TRY(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

    // callbacks
    LUCAS_ESP_TRY(esp_bt_gap_register_callback(gap_cb));
    LUCAS_ESP_TRY(esp_spp_register_callback(spp_cb));

    // init
    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    LUCAS_ESP_TRY(esp_spp_enhanced_init(&spp_cfg));

    return ESP_OK;
}
