#include "bt.h"
#include <string.h>
#include <nvs_flash.h>
#include <esp_bt.h>
#include <esp_gatt_common_api.h>
#include <esp_gatt_defs.h>
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
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

#define REMOTE_NOTIFY_CHAR_UUID 0xFF01

#define DIFLUID_SERVICE_UUID 0x00EE
#define DIFLUID_DEVICE_NAME "Microbalance"

struct gattc_profile_inst {
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t char_handle;
    esp_bd_addr_t remote_bda;
};

static struct gattc_profile_inst g_gattc = { .gattc_if = ESP_GATT_IF_NONE };

static char* bda2str(uint8_t* bda) {
    static char output[18] = { 0 };

    uint8_t* p = bda;
    sprintf(output, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return output;
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    switch (event) {
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        LOGI("ESP_BT_GAP_DISC_STATE_CHANGED_EVT");
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            LOGI("authentication succeeded: %s bda:[%s]", param->auth_cmpl.device_name, bda2str(param->auth_cmpl.bda));
        } else {
            LOGE("authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        LOGI("ESP_BT_GAP_MODE_CHG_EVT mode:%d bda:[%s]", param->mode_chg.mode, bda2str(param->mode_chg.bda));
        break;
    default:
        LOGW("unhandled gap event: %d", event);
        break;
    }
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
    switch (event) {
    case ESP_SPP_WRITE_EVT:
        LOGI("SPP_WRITE_EVT [%d]", param->write.len);
        if (param->write.status == ESP_SPP_SUCCESS) {
            lucas_event_t event = {
                .type = LUCAS_EVENT_SPP_WRITE_SUCCEEDED,
                .write_succeeded = {.num_bytes_written = param->write.len,
                                    .congested = param->write.cong}
            };
            lucas_event_send(&event);
        } else {
            LOGE("write failed");
            if (!param->cong.cong) {
                // the write failed but not because of congestion, try again straight away.
                // otherwise, on congestion, we'll retry when `ESP_SPP_CONG_EVT` tells us that the congestion is over
                LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_WRITE_AGAIN);
            }
        }

        g_shared_ctx.spp_handle = param->write.handle;
        if (param->write.cong)
            LOGE("congested!");
        break;
    case ESP_SPP_DATA_IND_EVT: {
        LOGI("SPP_DATA_IND_EVT [%d]", param->data_ind.len);

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
        LOGI("ESP_SPP_CONG_EVT");
        if (param->cong.status == ESP_SPP_SUCCESS) {
            // congestion is over!
            if (!param->cong.cong)
                LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CONGESTION_ENDED);
        }

        g_shared_ctx.spp_handle = param->cong.handle;
        break;
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            LOGI("ESP_SPP_INIT_EVT");

            LOG_ERR(esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, SERVER_NAME));

            LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
        } else {
            LOGE("ESP_SPP_INIT_EVT status:%d", param->init.status);
        }
        break;
    case ESP_SPP_START_EVT:
        LOGI("ESP_SPP_START_EVT handle:%lu sec_id:%d scn:%d", param->start.handle, param->start.sec_id, param->start.scn);
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        LOGI("ESP_SPP_SRV_OPEN_EVT status:%d handle:%lu rem_bda:[%s]", param->srv_open.status, param->srv_open.handle, bda2str(param->srv_open.rem_bda));
        LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CLEAR_BUFFER);
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
        g_shared_ctx.spp_handle = 0;
        break;
    case ESP_SPP_SRV_STOP_EVT:
        LOGI("ESP_SPP_SRV_STOP_EVT");
        g_shared_ctx.spp_handle = 0;
        break;
    case ESP_SPP_UNINIT_EVT:
        LOGI("ESP_SPP_UNINIT_EVT");
        g_shared_ctx.spp_handle = 0;
        break;
    default:
        LOGW("unhandled spp event: %d", event);
        break;
    }
}

static void gap_ble_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
            LOG_ERR(esp_ble_gap_start_scanning(30));
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
            LOGI("ble scan started");
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            uint8_t device_name_len = 0;
            const char* device_name = (const char*)esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &device_name_len);
            if (device_name != NULL && param->scan_rst.rssi >= -50) {
                LOGI("found device \"%.*s\"", device_name_len, device_name);
                if (strstr(device_name, DIFLUID_DEVICE_NAME) != NULL) {
                    LOG_ERR(esp_ble_gap_set_prefer_conn_params(param->scan_rst.bda, 32, 32, 0, 600));
                    LOG_ERR(esp_ble_gap_stop_scanning());
                    LOG_ERR(esp_ble_gattc_open(g_gattc.gattc_if, param->scan_rst.bda, param->scan_rst.ble_addr_type, true));
                }
            }
        }
        break;
    default:
        LOGW("unhandled ble gap event: %d", event);
        break;
    }
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
    static esp_ble_scan_params_t ble_scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };

    static esp_bt_uuid_t remote_filter_service_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = { .uuid16 = DIFLUID_SERVICE_UUID },
    };

    static esp_bt_uuid_t remote_filter_char_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = { .uuid16 = REMOTE_NOTIFY_CHAR_UUID },
    };

    static esp_bt_uuid_t notify_descr_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
    };

    switch (event) {
    case ESP_GATTC_REG_EVT:
        LOGI("ESP_GATTC_REG_EVT");
        if (param->reg.status == ESP_GATT_OK) {
            LOGI("registering gatt client, app_id: %hu, if: %hu", param->reg.app_id, gattc_if);
            g_gattc.gattc_if = gattc_if;
            LOG_ERR(esp_ble_gap_set_scan_params(&ble_scan_params));
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        LOGE("ESP_GATTC_OPEN_EVT - status = 0x%x", param->open.status);
        if (param->open.status == ESP_GATT_OK)
            LOGI("successfully opened");
        break;
    case ESP_GATTC_CONNECT_EVT:
        LOGI("ESP_GATTC_CONNECT_EVT conn_id %d, if %d", param->connect.conn_id, gattc_if);

        g_gattc.conn_id = param->connect.conn_id;
        memcpy(g_gattc.remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));

        LOG_ERR(esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id));
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        LOGE("ESP_GATTC_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        LOGI("ESP_GATTC_CFG_MTU_EVT");
        if (param->cfg_mtu.status != ESP_GATT_OK)
            LOGI("ESP_GATTC_CFG_MTU_EVT, Status %d, MTU %d, conn_id %d", param->cfg_mtu.status, param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
        break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (param->dis_srvc_cmpl.status != ESP_GATT_OK) {
            LOGE("discover service failed, status %d", param->dis_srvc_cmpl.status);
            break;
        }
        LOGI("discover service complete conn_id %d", param->dis_srvc_cmpl.conn_id);
        LOG_ERR(esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &remote_filter_service_uuid));
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        LOGI("SEARCH RES: conn_id = %x is primary service %d", param->search_res.conn_id, param->search_res.is_primary);
        LOGI("start handle %d end handle %d current handle value %d", param->search_res.start_handle, param->search_res.end_handle, param->search_res.srvc_id.inst_id);

        LOGI("service found");
        LOGI("UUID16: %x", param->search_res.srvc_id.uuid.uuid.uuid16);
        g_gattc.service_start_handle = param->search_res.start_handle;
        g_gattc.service_end_handle = param->search_res.end_handle;
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            LOGE("search service failed, error status = %x", param->search_cmpl.status);
            break;
        }

        if (param->search_cmpl.searched_service_source == ESP_GATT_SERVICE_FROM_REMOTE_DEVICE) {
            LOGI("got service information from remote device");
        } else if (param->search_cmpl.searched_service_source == ESP_GATT_SERVICE_FROM_NVS_FLASH) {
            LOGI("got service information from flash");
        } else {
            LOGI("unknown service source");
        }

        uint16_t count = 0;
        LOG_ERR(esp_ble_gattc_get_attr_count(gattc_if,
                                             param->search_cmpl.conn_id,
                                             ESP_GATT_DB_CHARACTERISTIC,
                                             g_gattc.service_start_handle,
                                             g_gattc.service_end_handle,
                                             ESP_GATT_ILLEGAL_HANDLE,
                                             &count));

        if (count == 0)
            break;

        esp_gattc_char_elem_t* char_elem_result = malloc(sizeof(esp_gattc_char_elem_t) * count);
        assert(char_elem_result);

        LOG_ERR(esp_ble_gattc_get_char_by_uuid(gattc_if,
                                               param->search_cmpl.conn_id,
                                               g_gattc.service_start_handle,
                                               g_gattc.service_end_handle,
                                               remote_filter_char_uuid,
                                               char_elem_result,
                                               &count));

        if (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
            g_gattc.char_handle = char_elem_result[0].char_handle;
            LOG_ERR(esp_ble_gattc_register_for_notify(gattc_if, g_gattc.remote_bda, char_elem_result[0].char_handle));
        }

        free(char_elem_result);

        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        LOGI("ESP_GATTC_REG_FOR_NOTIFY_EVT");
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            LOGE("REG FOR NOTIFY failed: error status = %d", param->reg_for_notify.status);
            break;
        }

        uint8_t buf[5] = { 3 };
        LOG_ERR(esp_ble_gattc_write_char(g_gattc.gattc_if,
                                         g_gattc.conn_id,
                                         g_gattc.char_handle,
                                         sizeof(buf),
                                         buf,
                                         ESP_GATT_WRITE_TYPE_NO_RSP,
                                         ESP_GATT_AUTH_REQ_NONE));

        uint16_t count = 0;
        LOG_ERR(esp_ble_gattc_get_attr_count(gattc_if,
                                             g_gattc.conn_id,
                                             ESP_GATT_DB_DESCRIPTOR,
                                             g_gattc.service_start_handle,
                                             g_gattc.service_end_handle,
                                             g_gattc.char_handle,
                                             &count));

        if (count == 0)
            break;

        esp_gattc_descr_elem_t* descr_elem_result = malloc(sizeof(esp_gattc_descr_elem_t) * count);
        assert(descr_elem_result);

        LOG_ERR(esp_ble_gattc_get_descr_by_char_handle(gattc_if,
                                                       g_gattc.conn_id,
                                                       param->reg_for_notify.handle,
                                                       notify_descr_uuid,
                                                       descr_elem_result,
                                                       &count));

        const uint16_t notify_en = 1;
        if (descr_elem_result[0].uuid.len == ESP_UUID_LEN_16 && descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
            LOG_ERR(esp_ble_gattc_write_char_descr(gattc_if,
                                                   g_gattc.conn_id,
                                                   descr_elem_result[0].handle,
                                                   sizeof(notify_en),
                                                   (uint8_t*)&notify_en,
                                                   ESP_GATT_WRITE_TYPE_RSP,
                                                   ESP_GATT_AUTH_REQ_NONE));
        }

        free(descr_elem_result);

        break;
    }
    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.is_notify) {
            LOGI("ESP_GATTC_NOTIFY_EVT, receive notify value:");
        } else {
            LOGI("ESP_GATTC_NOTIFY_EVT, receive indicate value:");
        }
        esp_log_buffer_hex(LUCAS_LOG_TAG, param->notify.value, param->notify.value_len);
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            LOGE("write descr failed, error status = %x", param->write.status);
            break;
        }
        LOGI("write descr success ");
        break;
    case ESP_GATTC_SRVC_CHG_EVT: {
        LOGI("ESP_GATTC_SRVC_CHG_EVT, bd_addr: %s", bda2str(param->srvc_chg.remote_bda));
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            LOGE("write char failed, error status = %x", param->write.status);
            break;
        }
        LOGI("write char success ");
        break;
    default:
        LOGW("unhadled gatcc event: %d", event);
        break;
    }
}

esp_err_t init_classic_bt() {
    // device
    TRY(esp_bt_dev_set_device_name(DEVICE_NAME));

    // gap
    esp_bt_pin_code_t pin_code = PIN;
    TRY(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, strlen(PIN), pin_code));
    TRY(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

    // callbacks
    TRY(esp_bt_gap_register_callback(bt_gap_cb));
    TRY(esp_spp_register_callback(spp_cb));

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    TRY(esp_spp_enhanced_init(&spp_cfg));

    return ESP_OK;
}

esp_err_t init_ble() {
    // callbacks
    TRY(esp_ble_gap_register_callback(gap_ble_cb));
    TRY(esp_ble_gattc_register_callback(gattc_cb));

    // gattc
    TRY(esp_ble_gattc_app_register(0));
    TRY(esp_ble_gatt_set_local_mtu(512));

    return ESP_OK;
}

esp_err_t lucas_bt_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    TRY(esp_bt_controller_init(&bt_cfg));
    TRY(esp_bt_controller_enable(bt_cfg.mode));

    TRY(esp_bluedroid_init());
    TRY(esp_bluedroid_enable());

    TRY(init_classic_bt());
    TRY(init_ble());

    return ESP_OK;
}
