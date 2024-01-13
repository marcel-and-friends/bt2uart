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
#define DIFLUID_SERVICE_UUID 0x00EE
#define DIFLUID_CHAR_UUID 0xAA01

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
        if (param->cong.status == ESP_SPP_SUCCESS) {
            // congestion is over!
            if (!param->cong.cong)
                LUCAS_EVENT_SEND(LUCAS_EVENT_SPP_CONGESTION_ENDED);
        }

        g_shared_ctx.spp_handle = param->cong.handle;
        break;
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            LUCAS_LOGI("ESP_SPP_INIT_EVT");

            ESP_ERROR_CHECK(esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, SERVER_NAME));
            // esp_ble_gap_start_scanning(30);

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

static bool get_name_from_eir(uint8_t* eir, char* bdname, uint8_t* bdname_len) {
    assert(eir && bdname && bdname_len);

    uint8_t rmt_bdname_len = 0;
    uint8_t* rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname)
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN)
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;

        *bdname_len = rmt_bdname_len;

        memcpy(bdname, rmt_bdname, rmt_bdname_len);
        bdname[rmt_bdname_len] = '\0';

        return true;
    }

    return false;
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    char bda_str[18] = { 0 };

    switch (event) {
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        LUCAS_LOGI("ESP_BT_GAP_DISC_STATE_CHANGED_EVT");
        break;
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

static void gap_ble_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    static esp_bd_addr_t microbalance_bda = { 0 };
    static esp_ble_addr_type_t microbalance_ble_addr_type = 0;
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
            esp_ble_gap_start_scanning(30);
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
            LUCAS_LOGI("ble scan started");
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            uint8_t device_name_len = 0;
            const char* device_name = (const char*)esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &device_name_len);
            if (device_name != NULL) {
                LUCAS_LOGI("found device \"%.*s\"", device_name_len, device_name);
                if (strstr(device_name, "Microbalance") != NULL) {
                    memcpy(microbalance_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
                    microbalance_ble_addr_type = param->scan_rst.ble_addr_type;
                    esp_ble_gap_stop_scanning();
                }
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            LUCAS_LOGI("ble scan stopped");
            esp_ble_gattc_open(g_gattc.gattc_if, microbalance_bda, microbalance_ble_addr_type, true);
        }
        break;
    default:
        LUCAS_LOGW("unhandled ble gap event: %d", event);
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
        .uuid = { .uuid16 = DIFLUID_CHAR_UUID },
    };

    static esp_bt_uuid_t notify_descr_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
    };

    switch (event) {
    case ESP_GATTC_REG_EVT:
        LUCAS_LOGI("ESP_GATTC_REG_EVT");
        if (param->reg.status == ESP_GATT_OK) {
            LUCAS_LOGI("registering gatt client, app_id: %hu, if: %hu", param->reg.app_id, gattc_if);
            g_gattc.gattc_if = gattc_if;
            esp_ble_gap_set_scan_params(&ble_scan_params);
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        LUCAS_LOGE("ESP_GATTC_OPEN_EVT - status = 0x%x", param->open.status);
        if (param->open.status == ESP_GATT_OK)
            LUCAS_LOGI("successfully opened");
        break;
    case ESP_GATTC_CONNECT_EVT:
        LUCAS_LOGI("ESP_GATTC_CONNECT_EVT conn_id %d, if %d", param->connect.conn_id, gattc_if);

        g_gattc.conn_id = param->connect.conn_id;
        memcpy(g_gattc.remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        LUCAS_LOGI("REMOTE BDA:");
        esp_log_buffer_hex(LUCAS_LOG_TAG, g_gattc.remote_bda, sizeof(esp_bd_addr_t));

        esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id);
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        LUCAS_LOGE("ESP_GATTC_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        LUCAS_LOGI("ESP_GATTC_CFG_MTU_EVT");
        if (param->cfg_mtu.status != ESP_GATT_OK)
            LUCAS_LOGI("ESP_GATTC_CFG_MTU_EVT, Status %d, MTU %d, conn_id %d", param->cfg_mtu.status, param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
        break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (param->dis_srvc_cmpl.status != ESP_GATT_OK) {
            LUCAS_LOGE("discover service failed, status %d", param->dis_srvc_cmpl.status);
            break;
        }
        LUCAS_LOGI("discover service complete conn_id %d", param->dis_srvc_cmpl.conn_id);
        esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &remote_filter_service_uuid);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        LUCAS_LOGI("SEARCH RES: conn_id = %x is primary service %d", param->search_res.conn_id, param->search_res.is_primary);
        LUCAS_LOGI("start handle %d end handle %d current handle value %d", param->search_res.start_handle, param->search_res.end_handle, param->search_res.srvc_id.inst_id);
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16) {
            LUCAS_LOGI("service found");
            LUCAS_LOGI("UUID16: %x", param->search_res.srvc_id.uuid.uuid.uuid16);
            g_gattc.service_start_handle = param->search_res.start_handle;
            g_gattc.service_end_handle = param->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            LUCAS_LOGE("search service failed, error status = %x", param->search_cmpl.status);
            break;
        }

        if (param->search_cmpl.searched_service_source == ESP_GATT_SERVICE_FROM_REMOTE_DEVICE) {
            LUCAS_LOGI("Get service information from remote device");
        } else if (param->search_cmpl.searched_service_source == ESP_GATT_SERVICE_FROM_NVS_FLASH) {
            LUCAS_LOGI("Get service information from flash");
        } else {
            LUCAS_LOGI("unknown service source");
        }
        LUCAS_LOGI("ESP_GATTC_SEARCH_CMPL_EVT");
        uint16_t count = 0;
        esp_ble_gattc_get_attr_count(gattc_if,
                                     param->search_cmpl.conn_id,
                                     ESP_GATT_DB_CHARACTERISTIC,
                                     g_gattc.service_start_handle,
                                     g_gattc.service_end_handle,
                                     ESP_GATT_ILLEGAL_HANDLE,
                                     &count);

        if (count > 0) {
            esp_gattc_char_elem_t* char_elem_result = malloc(sizeof(esp_gattc_char_elem_t) * count);
            assert(char_elem_result);

            // esp_ble_gattc_get_char_by_uuid(gattc_if,
            //                                param->search_cmpl.conn_id,
            //                                g_gattc_profile.service_start_handle,
            //                                g_gattc_profile.service_end_handle,
            //                                remote_filter_char_uuid,
            //                                char_elem_result,
            //                                &count);

            // /*  Every service have only one char in our 'ESP_GATTS_DEMO' demo, so we used first 'char_elem_result' */
            // if (count > 0 && (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
            //     g_gattc_profile.char_handle = char_elem_result[0].char_handle;
            //     esp_ble_gattc_register_for_notify(gattc_if, g_gattc_profile.remote_bda, char_elem_result[0].char_handle);
            // }

            free(char_elem_result);
        } else {
            LUCAS_LOGE("no char found");
        }
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        LUCAS_LOGI("ESP_GATTC_REG_FOR_NOTIFY_EVT");
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            LUCAS_LOGE("REG FOR NOTIFY failed: error status = %d", param->reg_for_notify.status);
        } else {
            uint16_t count = 0;
            esp_ble_gattc_get_attr_count(gattc_if,
                                         g_gattc.conn_id,
                                         ESP_GATT_DB_DESCRIPTOR,
                                         g_gattc.service_start_handle,
                                         g_gattc.service_end_handle,
                                         g_gattc.char_handle,
                                         &count);

            if (count > 0) {
                esp_gattc_descr_elem_t* descr_elem_result = malloc(sizeof(esp_gattc_descr_elem_t) * count);
                assert(descr_elem_result);

                esp_ble_gattc_get_descr_by_char_handle(gattc_if,
                                                       g_gattc.conn_id,
                                                       param->reg_for_notify.handle,
                                                       notify_descr_uuid,
                                                       descr_elem_result,
                                                       &count);

                // /* Every char has only one descriptor in our 'ESP_GATTS_DEMO' demo, so we used first 'descr_elem_result' */
                // if (count > 0 && descr_elem_result[0].uuid.len == ESP_UUID_LEN_16 && descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                //     esp_ble_gattc_write_char_descr(gattc_if,
                //                                    g_gattc_profile.conn_id,
                //                                    descr_elem_result[0].handle,
                //                                    sizeof(notify_en),
                //                                    (uint8_t*)&notify_en,
                //                                    ESP_GATT_WRITE_TYPE_RSP,
                //                                    ESP_GATT_AUTH_REQ_NONE);
                // }

                free(descr_elem_result);
            } else {
                LUCAS_LOGE("decsr not found");
            }
        }
        break;
    }
    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.is_notify) {
            LUCAS_LOGI("ESP_GATTC_NOTIFY_EVT, receive notify value:");
        } else {
            LUCAS_LOGI("ESP_GATTC_NOTIFY_EVT, receive indicate value:");
        }
        esp_log_buffer_hex(LUCAS_LOG_TAG, param->notify.value, param->notify.value_len);
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            LUCAS_LOGE("write descr failed, error status = %x", param->write.status);
            break;
        }
        LUCAS_LOGI("write descr success ");
        uint8_t write_char_data[35];
        for (int i = 0; i < sizeof(write_char_data); ++i) {
            write_char_data[i] = i % 256;
        }
        esp_ble_gattc_write_char(gattc_if,
                                 g_gattc.conn_id,
                                 g_gattc.char_handle,
                                 sizeof(write_char_data),
                                 write_char_data,
                                 ESP_GATT_WRITE_TYPE_RSP,
                                 ESP_GATT_AUTH_REQ_NONE);
        break;
    case ESP_GATTC_SRVC_CHG_EVT: {
        esp_bd_addr_t bda;
        memcpy(bda, param->srvc_chg.remote_bda, sizeof(esp_bd_addr_t));
        LUCAS_LOGI("ESP_GATTC_SRVC_CHG_EVT, bd_addr:");
        esp_log_buffer_hex(LUCAS_LOG_TAG, bda, sizeof(esp_bd_addr_t));
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            LUCAS_LOGE("write char failed, error status = %x", param->write.status);
            break;
        }
        LUCAS_LOGI("write char success ");
        break;
    default:
        LUCAS_LOGW("unhadled gatcc event: %d", event);
        break;
    }
}

esp_err_t init_classic_bt() {
    // device
    LUCAS_ESP_TRY(esp_bt_dev_set_device_name(DEVICE_NAME));

    // gap
    esp_bt_pin_code_t pin_code = PIN;
    LUCAS_ESP_TRY(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, strlen(PIN), pin_code));
    LUCAS_ESP_TRY(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

    // callbacks
    LUCAS_ESP_TRY(esp_bt_gap_register_callback(gap_cb));
    LUCAS_ESP_TRY(esp_spp_register_callback(spp_cb));

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    LUCAS_ESP_TRY(esp_spp_enhanced_init(&spp_cfg));

    return ESP_OK;
}

esp_err_t init_ble() {
    // callbacks
    LUCAS_ESP_TRY(esp_ble_gap_register_callback(gap_ble_cb));
    LUCAS_ESP_TRY(esp_ble_gattc_register_callback(gattc_cb));

    // gattc
    LUCAS_ESP_TRY(esp_ble_gattc_app_register(0));
    LUCAS_ESP_TRY(esp_ble_gatt_set_local_mtu(512));

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
    LUCAS_ESP_TRY(esp_bt_controller_init(&bt_cfg));
    LUCAS_ESP_TRY(esp_bt_controller_enable(bt_cfg.mode));

    esp_bluedroid_config_t bluedroid_cfg = { .ssp_en = false };
    LUCAS_ESP_TRY(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    LUCAS_ESP_TRY(esp_bluedroid_enable());

    LUCAS_ESP_TRY(init_classic_bt());
    // LUCAS_ESP_TRY(init_ble());

    return ESP_OK;
}
