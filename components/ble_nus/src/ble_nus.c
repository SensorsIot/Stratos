#include "ble_nus.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "sonde_types.h"
#include "stratos_codec.h"

static const char *TAG = "ble";

/* Nordic UART Service UUIDs — see FSD §6.1.1 R-1.
   On-wire payload format is Stratos's, designed wire-compatible with the
   MySondyGo API v3.0 ASCII protocol so existing clients connect unchanged. */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);
static const ble_uuid128_t NUS_RX  = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E);
static const ble_uuid128_t NUS_TX  = BLE_UUID128_INIT(
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);

static uint16_t s_conn_handle = 0xFFFF;
static uint16_t s_tx_attr_handle;
static uint8_t  s_addr_type;
static char     s_dev_name[32];

static int gatt_rx_write(uint16_t conn_h, uint16_t attr_h,
                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gatt_tx_access(uint16_t conn_h, uint16_t attr_h,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid       = &NUS_RX.u,
                .access_cb  = gatt_rx_write,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &NUS_TX.u,
                .access_cb  = gatt_tx_access,
                .val_handle = &s_tx_attr_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static int notify_emit(const char *bytes, size_t len, void *ctx)
{
    (void)ctx;
    if (s_conn_handle == 0xFFFF) return 0;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(bytes, len);
    if (!om) return -1;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify rc=%d", rc);
    return rc;
}

static int gatt_rx_write(uint16_t conn_h, uint16_t attr_h,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_h; (void)attr_h; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t buf[256];
    if (len > sizeof(buf)) len = sizeof(buf);
    uint16_t copied = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, &copied);
    msg_codec_handle_input(buf, copied);
    return 0;
}

static int gatt_tx_access(uint16_t conn_h, uint16_t attr_h,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_h; (void)attr_h; (void)ctxt; (void)arg;
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected, handle=%u", s_conn_handle);
        } else {
            st_ble_start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = 0xFFFF;
        st_ble_start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        st_ble_start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribed handle=%d notify=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

void st_ble_start_advertising(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.tx_pwr_lvl_is_present = 1;
    f.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    f.name = (uint8_t *)s_dev_name;
    f.name_len = strlen(s_dev_name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params ap = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_start rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    st_ble_start_advertising();
    ESP_LOGI(TAG, "advertising as %s", s_dev_name);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "reset reason=%d", reason); }

static void host_task(void *param) { (void)param; nimble_port_run(); nimble_port_freertos_deinit(); }

static void on_sonde_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != SONDE_EVT_FRAME) return;
    msg_codec_emit_state((const sonde_frame_t *)data);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_conn_handle != 0xFFFF) {
            extern const sonde_frame_t *st_state_last(void);
            const sonde_frame_t *f = st_state_last();
            if (!f || f->state == SONDE_STATE_NO_SIGNAL) {
                msg_codec_emit_state(f);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t st_ble_init(const char *device_name)
{
    strlcpy(s_dev_name, device_name ? device_name : "Stratos", sizeof(s_dev_name));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc) return ESP_FAIL;
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc) return ESP_FAIL;
    rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc) return ESP_FAIL;

    msg_codec_init(notify_emit, NULL);

    esp_event_handler_register(SONDE_EVENT, SONDE_EVT_FRAME, on_sonde_evt, NULL);
    xTaskCreate(heartbeat_task, "ble_hb", 3072, NULL, 5, NULL);

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
