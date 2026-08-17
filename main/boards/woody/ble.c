#include "ble.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE";

/* 6E400001-A3B5-F393-E0A9-E50E24DCCA9E (NUS Service) */
static const ble_uuid128_t nus_svc_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xB5, 0xA3, 0x01, 0x00, 0x40, 0x6E);

/* 6E400002-A3B5-F393-E0A9-E50E24DCCA9E (RX Characteristic) */
static const ble_uuid128_t nus_rx_chr_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xB5, 0xA3, 0x02, 0x00, 0x40, 0x6E);

/* 6E400003-A3B5-F393-E0A9-E50E24DCCA9E (TX Characteristic) */
static const ble_uuid128_t nus_tx_chr_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xB5, 0xA3, 0x03, 0x00, 0x40, 0x6E);

static uint16_t nus_tx_handle;
static ble_rx_callback_t g_ble_rx_callback = NULL;
static int g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_advertising = false;
static bool g_ble_active = false;
static TaskHandle_t g_ble_host_task_handle = NULL;

static int nus_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_on_sync(void);
static void ble_host_task(void *param);

static const struct ble_gatt_svc_def nus_svcs[] = {

    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &nus_rx_chr_uuid.u,
                .access_cb = nus_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &nus_tx_chr_uuid.u,
                .access_cb = nus_chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_tx_handle,
            },
            {
                0, /* No more characteristics in this service */
            },
        }
    },
    {
        0, /* No more services */
    },
};

static int nus_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ble_uuid_cmp(ctxt->chr->uuid, &nus_rx_chr_uuid.u) == 0) {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        ESP_LOGD(TAG, "BLE RX: len=%d", om_len);

        // For large packets, the data might be fragmented across multiple mbufs.
        // We use a temporary buffer to flatten it.
        uint8_t *buf = malloc(om_len);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for BLE RX flattening");
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        uint16_t out_len;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, om_len, &out_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to flatten mbuf: %d", rc);
            free(buf);
            return BLE_ATT_ERR_UNLIKELY;
        }
        
        /* Handshake protocol: if "PING" received, reply "PONG" */
        if (out_len >= 4 && memcmp(buf, "PING", 4) == 0) {
            ESP_LOGI(TAG, "Handshake: PING received, replying PONG");
            ble_send_data((uint8_t *)"PONG", 4);
        }
        else if (g_ble_rx_callback) {
            g_ble_rx_callback(buf, out_len);
        }

        free(buf);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void ble_advertise(void)
{
    if (!g_ble_active) {
        ESP_LOGW(TAG, "ble_advertise called but BLE is not active");
        return;
    }
    ESP_LOGI(TAG, "ble_advertise called");
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    const char *name;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = "SmartClock"; // Set a custom device name for BLE advertisement
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ESP_LOGI(TAG, "Setting advertisement fields to name: %s", name);
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting advertisement data; rc=%d", rc);
        return;
    }

    // Set scan response fields to advertise the service UUID
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = &nus_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    ESP_LOGI(TAG, "Setting scan response fields for service UUID");
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting scan response data; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // Set moderate advertising interval to balance discovery speed and interference
    adv_params.itvl_min = 160;  // 160 * 0.625ms = 100ms
    adv_params.itvl_max = 320;  // 320 * 0.625ms = 200ms

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "error determining address type; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Starting GAP advertisement (own_addr_type=%d)...", own_addr_type);
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "error enabling advertisement; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "GAP advertisement started successfully");
    g_advertising = true;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connected; status=%d ", event->connect.status);
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
        }
        g_advertising = false;
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d ", event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (g_ble_active) {
            ble_advertise();
        }
        
        // Notify application to resume voice processing if BLE transfer was active
        extern bool g_ble_transfer_active;
        if (g_ble_transfer_active) {
            extern void restore_xiaozhi_after_ble_transfer(void);
            restore_xiaozhi_after_ble_transfer();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete; reason=%d ", event->adv_complete.reason);
        g_advertising = false;
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe; cur_notify=%d", event->subscribe.cur_notify);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update; conn_handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;
    }
    return 0;
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "ble_on_sync called, setting address and starting advertising");
    if (!g_ble_active) {
        ESP_LOGW(TAG, "ble_on_sync called but BLE is not active");
        return;
    }
    int rc;
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    ble_advertise();
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    ESP_LOGI(TAG, "nimble_port_run returned, waiting for deletion");
    vTaskDelay(portMAX_DELAY);
}

esp_err_t ble_init(void)
{
    ESP_LOGI(TAG, "ble_init started");
    int rc;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble %d", ret);
        return ret;
    }

    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    rc = ble_gatts_count_cfg(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("SmartClock");
    assert(rc == 0);

    g_ble_active = true;

    ESP_LOGI(TAG, "Creating FreeRTOS BLE host task manually...");
    BaseType_t xRet = xTaskCreatePinnedToCore(
        ble_host_task,
        "ble_hs",
        8192,
        NULL,
        15, // Priority 15
        &g_ble_host_task_handle,
        1   // Pinned to Core 1
    );
    if (xRet != pdPASS) {
        g_ble_active = false;
        g_ble_host_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create BLE host task manually: %d", xRet);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "FreeRTOS BLE host task manually created successfully");

    return ESP_OK;
}

esp_err_t ble_deinit(void)
{
    ESP_LOGI(TAG, "ble_deinit called, cleaning up NimBLE");
    g_ble_active = false;

    if (g_advertising) {
        ble_gap_adv_stop();
        g_advertising = false;
    }
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    ESP_LOGI(TAG, "Stopping NimBLE port...");
    int rc = nimble_port_stop();
    if (rc != 0 && rc != 2) { // 2 is BLE_HS_EALREADY (already stopped/not running)
        ESP_LOGE(TAG, "nimble_port_stop failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "NimBLE port stopped successfully");
    }

    if (g_ble_host_task_handle != NULL) {
        ESP_LOGI(TAG, "Deleting BLE host task...");
        vTaskDelete(g_ble_host_task_handle);
        g_ble_host_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Deinitializing NimBLE port...");
    nimble_port_deinit();
    ESP_LOGI(TAG, "NimBLE port deinitialized");

    return ESP_OK;
}



esp_err_t ble_start(void)
{
    if (!g_advertising && g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ble_advertise();
    }
    return ESP_OK;
}

esp_err_t ble_stop(void)
{
    if (g_advertising) {
        ble_gap_adv_stop();
        g_advertising = false;
    }
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return ESP_OK;
}

esp_err_t ble_send_data(uint8_t *data, uint16_t len)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_conn_handle == 0xFFFF) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, nus_tx_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void ble_set_rx_callback(ble_rx_callback_t callback)
{
    g_ble_rx_callback = callback;
}

esp_err_t ble_update_conn_params(bool high_speed)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_conn_handle == 0xFFFF) {
        ESP_LOGW(TAG, "Cannot update conn params: no active connection");
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_gap_upd_params params;
    memset(&params, 0, sizeof(params));

    if (high_speed) {
        // High speed mode connection interval (min 7.5ms, max 15ms)
        // 6 * 1.25ms = 7.5ms, 12 * 1.25ms = 15ms
        params.itvl_min = 6;
        params.itvl_max = 12;
        params.latency = 0;
        params.supervision_timeout = 400; // 400 * 10ms = 4 seconds
    } else {
        // Normal/idle mode connection interval (min 50ms, max 100ms)
        params.itvl_min = 40;
        params.itvl_max = 80;
        params.latency = 0;
        params.supervision_timeout = 400;
    }

    int rc = ble_gap_update_params(g_conn_handle, &params);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to request connection params update; rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Requested connection parameter update (high_speed=%d)", high_speed);
    return ESP_OK;
}

bool ble_is_connected(void)
{
    return g_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
