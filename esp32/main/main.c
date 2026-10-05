#include <assert.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "reader.h"
#include "relay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEVICE_NAME "homekey-reader"
static const char *TAG = "homekey-reader";
/* UUID bytes are least-significant first in NimBLE.
 * Service:		7a6b0001-5b21-4f36-8e5d-9c3a26d74210
 * Characteristic: 7a6b0002-5b21-4f36-8e5d-9c3a26d74210
 */
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
	0x10, 0x42, 0xd7, 0x26, 0x3a, 0x9c, 0x5d, 0x8e, 0x36, 0x4f, 0x21, 0x5b, 0x01, 0x00, 0x6b, 0x7a);
static const ble_uuid128_t ping_uuid = BLE_UUID128_INIT(
	0x10, 0x42, 0xd7, 0x26, 0x3a, 0x9c, 0x5d, 0x8e, 0x36, 0x4f, 0x21, 0x5b, 0x02, 0x00, 0x6b, 0x7a);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
	0x10, 0x42, 0xd7, 0x26, 0x3a, 0x9c, 0x5d, 0x8e, 0x36, 0x4f, 0x21, 0x5b, 0x03, 0x00, 0x6b, 0x7a);
static const ble_uuid128_t apdu_uuid = BLE_UUID128_INIT(
	0x10, 0x42, 0xd7, 0x26, 0x3a, 0x9c, 0x5d, 0x8e, 0x36, 0x4f, 0x21, 0x5b, 0x04, 0x00, 0x6b, 0x7a);
static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn;
	(void)attr;
	(void)arg;
	if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
		ESP_LOGW(TAG, "Unexpected status access: op=%u conn=%u attr=%u",
				(unsigned)ctxt->op,
				(unsigned)conn,
				(unsigned)attr);
		return BLE_ATT_ERR_UNLIKELY;
	}
	uint8_t data[18];
	size_t n = relay_status(data);
	return os_mbuf_append(ctxt->om, data, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static int apdu_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn;
	(void)attr;
	(void)arg;
	uint8_t data[RELAY_VALUE_MAX];
	if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
		size_t n = relay_response(data);
		return os_mbuf_append(ctxt->om, data, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
		size_t n = OS_MBUF_PKTLEN(ctxt->om);
		if (n > sizeof(data) || n < RELAY_HEADER)
			return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
		if (os_mbuf_copydata(ctxt->om, 0, n, data) != 0)
			return BLE_ATT_ERR_UNLIKELY;
		return relay_write(data, n) ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
	}
	return BLE_ATT_ERR_UNLIKELY;
}
static uint8_t own_addr_type;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
/* Accessed only by the NimBLE host task. */
static uint8_t reply[8];
static bool have_reply;

static int ping_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn;
	(void)attr;
	(void)arg;
	if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
		uint8_t request[8];
		if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(request))
			return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
		if (os_mbuf_copydata(ctxt->om, 0, sizeof(request), request) != 0)
			return BLE_ATT_ERR_UNLIKELY;
		if (memcmp(request, "PING", 4) != 0)
			return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
		memcpy(reply, request, sizeof(reply));
		memcpy(reply, "PONG", 4);
		have_reply = true;
		ESP_LOGI(TAG, "BLE PING received; PONG ready");
		return 0;
	}
	if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
		if (!have_reply)
			return 0; /* Empty until the first PING. */
		return os_mbuf_append(ctxt->om, reply, sizeof(reply)) == 0 ? 0
																   : BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	return BLE_ATT_ERR_UNLIKELY;
}
static const struct ble_gatt_svc_def services[] = {
	{.type = BLE_GATT_SVC_TYPE_PRIMARY,
	 .uuid = &service_uuid.u,
	 .characteristics =
		 (struct ble_gatt_chr_def[]){
			 {.uuid = &ping_uuid.u,
			  .access_cb = ping_access,
			  .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE},
			 {.uuid = &status_uuid.u, .access_cb = status_access, .flags = BLE_GATT_CHR_F_READ},
			 {.uuid = &apdu_uuid.u,
			  .access_cb = apdu_access,
			  .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE},
			 {0}}},
	{0}};
static void advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg)
{
	(void)arg;
	ESP_LOGI(TAG, "GAP event=%d (CONNECT=%d DISCONNECT=%d)",
		 event->type,
		 BLE_GAP_EVENT_CONNECT,
		 BLE_GAP_EVENT_DISCONNECT);
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status == 0) {
			connection = event->connect.conn_handle;
			relay_connected(true);
			have_reply = false;
			ESP_LOGI(TAG, "BLE gateway connected");
		} else
			advertise();
		break;
	case BLE_GAP_EVENT_DISCONNECT:
		relay_connected(false);
		connection = BLE_HS_CONN_HANDLE_NONE;
		have_reply = false;
		ESP_LOGI(TAG, "BLE gateway disconnected: %d", event->disconnect.reason);
		advertise();
		break;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		if (connection == BLE_HS_CONN_HANDLE_NONE)
			advertise();
		break;
	default:
		break;
	}
	return 0;
}
static void advertise(void)
{
	struct ble_hs_adv_fields fields = {0};
	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	fields.uuids128 = (ble_uuid128_t *)&service_uuid;
	fields.num_uuids128 = 1;
	fields.uuids128_is_complete = 1;
	int rc = ble_gap_adv_set_fields(&fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "advertisement fields: %d", rc);
		return;
	}
	struct ble_hs_adv_fields scan = {0};
	scan.name = (uint8_t *)DEVICE_NAME;
	scan.name_len = strlen(DEVICE_NAME);
	scan.name_is_complete = 1;
	rc = ble_gap_adv_rsp_set_fields(&scan);
	if (rc != 0) {
		ESP_LOGE(TAG, "scan response: %d", rc);
		return;
	}
	struct ble_gap_adv_params params = {0};
	params.conn_mode = BLE_GAP_CONN_MODE_UND;
	params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
	params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
	rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
	if (rc != 0)
		ESP_LOGE(TAG, "advertisement start: %d", rc);
}
/* This callback runs in the NFC owner task; it holds the target active
 * until Go finishes authentication or the relay session expires. */
static bool report_card(const nfc_card_t *card)
{
	ESP_LOGI(TAG, "NFC card detected; uid_len=%u", card->uid_len);
	ESP_LOG_BUFFER_HEX_LEVEL(TAG, card->uid, card->uid_len, ESP_LOG_INFO);
	return relay_card(card);
}
static void on_reset(int reason)
{
	relay_connected(false);
	connection = BLE_HS_CONN_HANDLE_NONE;
	have_reply = false;
	ESP_LOGE(TAG, "NimBLE reset: %d", reason);
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
	ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2],
			 addr[1], addr[0]);
	advertise();
}
static void host_task(void *arg)
{
	(void)arg;
	nimble_port_run();
	nimble_port_freertos_deinit();
}
static void nfc_task(void *arg)
{
	(void)arg;
	start_polling(report_card);
	vTaskDelete(NULL);
}

void app_main(void)
{
	relay_init();
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	ESP_ERROR_CHECK(nimble_port_init());
	ble_hs_cfg.reset_cb = on_reset;
	ble_hs_cfg.sync_cb = on_sync;
	ble_svc_gap_init();
	ble_svc_gatt_init();
	int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
	assert(rc == 0);
	rc = ble_gatts_count_cfg(services);
	assert(rc == 0);
	rc = ble_gatts_add_svcs(services);
	assert(rc == 0);
	nimble_port_freertos_init(host_task);
	BaseType_t started = xTaskCreate(nfc_task, "pn532", 8192, NULL, 4, NULL);
	assert(started == pdPASS);
}
