#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#define DEVICE_NAME "homekey-reader"
#define ADV_DURATION_MS 2000

/*
 * Canonical UUID:
 *   7b1e0001-7b1e-4a11-9abc-0123456789ab
 *
 * BLE advertising puts 128-bit UUIDs on the wire least-significant byte first.
 */
static const uint8_t service_uuid_le[16] = {
    0xab, 0x89, 0x67, 0x45,
    0x23, 0x01, 0xbc, 0x9a,
    0x11, 0x4a, 0x1e, 0x7b,
    0x01, 0x00, 0x1e, 0x7b,
};

static const char *TAG = "homekey-reader";
static uint8_t own_addr_type;
static uint16_t mock_seq;

static void start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE central connected, handle=%u",
                     event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d",
                     event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE central disconnected, reason=%d",
                 event->disconnect.reason);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /*
         * Change the sequence number so Home Assistant sees changing
         * service data instead of deduplicating identical advertisements.
         */
        mock_seq++;
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void build_adv_data(uint8_t adv[29])
{
    size_t p = 0;

    /* Flags AD structure: general discoverable, BR/EDR not supported. */
    adv[p++] = 0x02;
    adv[p++] = 0x01;
    adv[p++] = 0x06;

    /*
     * 128-bit Service Data AD structure (type 0x21):
     *
     *   UUID[16]
     *   version[1]   = 1
     *   type[1]      = 1 (mock PN532 tag)
     *   seq[2]       = little endian
     *   data[4]      = fake NFCID1 / UID: DE:AD:BE:EF
     *
     * AD length is 1 byte type + 16 byte UUID + 8 byte payload = 25.
     */
    adv[p++] = 25;
    adv[p++] = 0x21;

    memcpy(&adv[p], service_uuid_le, sizeof(service_uuid_le));
    p += sizeof(service_uuid_le);

    adv[p++] = 1; /* protocol version */
    adv[p++] = 1; /* message type: mock PN532 tag */
    adv[p++] = (uint8_t)(mock_seq & 0xff);
    adv[p++] = (uint8_t)(mock_seq >> 8);
    adv[p++] = 0xde;
    adv[p++] = 0xad;
    adv[p++] = 0xbe;
    adv[p++] = 0xef;

    assert(p == 29);
}

static void start_advertising(void)
{
    uint8_t adv[29];
    struct ble_gap_adv_params params = {0};
    int rc;

    build_adv_data(adv);

    rc = ble_gap_adv_set_data(adv, sizeof(adv));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: %d", rc);
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(250);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(300);

    rc = ble_gap_adv_start(
        own_addr_type,
        NULL,
        ADV_DURATION_MS,
        &params,
        gap_event,
        NULL
    );
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG,
             "advertising mock PN532 packet: seq=%u uid=DE:AD:BE:EF",
             mock_seq);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc;
    uint8_t addr[6];

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);

    rc = ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    assert(rc == 0);

    ESP_LOGI(TAG,
             "BLE address %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    start_advertising();
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err;
    int rc;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    assert(rc == 0);

    ESP_LOGI(TAG, "starting BLE mock PN532 transmitter");
    nimble_port_freertos_init(host_task);
}
