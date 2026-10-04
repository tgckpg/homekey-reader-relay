#include <assert.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "reader.h"

#define DEVICE_NAME "homekey-reader"
#define ADV_DURATION_MS 1000
#define CARD_QUEUE_LENGTH 8

static const char *TAG = "homekey-reader";
static QueueHandle_t card_queue;
static struct ble_npl_event card_event;

/* The fields below belong exclusively to the NimBLE host task. */
static uint8_t own_addr_type;
static uint16_t event_seq;
static bool ble_ready;
static bool advertising;
static bool have_pending;
static nfc_card_t pending;

static void advertise_next(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        advertising = false;
        have_pending = false;
        ESP_LOGI(TAG, "card advertisement complete");
        /* Only start again if a different card event is waiting. */
        advertise_next();
    }
    return 0;
}

static void advertise_next(void)
{
    if (!ble_ready || advertising) {
        return;
    }
    while (have_pending || xQueueReceive(card_queue, &pending, 0) == pdTRUE) {
        if (!have_pending) {
            have_pending = true;
            ++event_seq;
        }
        uint8_t adv[31];
        size_t adv_len = build_card_adv(adv, &pending, event_seq);
        assert(adv_len > 0);

        int rc = ble_gap_adv_set_data(adv, (int)adv_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "setting advertisement failed: %d; event dropped", rc);
            have_pending = false;
            continue;
        }
        struct ble_gap_adv_params params = {0};
        params.conn_mode = BLE_GAP_CONN_MODE_NON;
        params.disc_mode = BLE_GAP_DISC_MODE_NON;
        params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
        params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);

        rc = ble_gap_adv_start(own_addr_type, NULL, ADV_DURATION_MS,
                               &params, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "starting advertisement failed: %d; event dropped", rc);
            have_pending = false;
            continue;
        }
        advertising = true;
        ESP_LOGI(TAG, "advertising card: event_seq=%u uid_len=%u",
                 event_seq, pending.uid_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, pending.uid, pending.uid_len, ESP_LOG_INFO);
        return;
    }
}

static void on_card_event(struct ble_npl_event *event)
{
    (void)event;
    advertise_next();
}

/* Called from PN532 task: copy the UID, then notify the host task.
 * No BLE calls or shared advertisement buffers cross task boundaries.
 */
static bool report_card(const nfc_card_t *card)
{
    if (xQueueSend(card_queue, card, 0) != pdTRUE) {
        ESP_LOGW(TAG, "card queue full; retrying while card is present");
        return false;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &card_event);
    return true;
}

static void on_reset(int reason)
{
    ble_ready = false;
    advertising = false;
    /* Keep a partially advertised event and its sequence for resync. */
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void on_sync(void)
{
    uint8_t addr[6];
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);
    rc = ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    assert(rc == 0);
    ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    ble_ready = true;
    advertise_next();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nimble_port_init());

    card_queue = xQueueCreate(CARD_QUEUE_LENGTH, sizeof(nfc_card_t));
    assert(card_queue != NULL);
    ble_npl_event_init(&card_event, on_card_event, NULL);
    /* Reduce duplicate sequence values following a reboot; not a unique ID. */
    event_seq = (uint16_t)esp_random();
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    assert(rc == 0);
    nimble_port_freertos_init(host_task);

    /* app_main is already a FreeRTOS task; UART waits yield to NimBLE. */
    start_polling(report_card);
}
