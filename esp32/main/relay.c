#include "relay.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SESSION_TIMEOUT_US (15000000LL)
typedef struct {
	uint32_t session;
	uint16_t seq, length;
	uint8_t data[RELAY_APDU_MAX];
} request_t;
static SemaphoreHandle_t mutex;
static QueueHandle_t requests;
static bool connected, active, busy, ending;
static uint32_t session_counter, session;
static nfc_card_t current;
static request_t assembling;
static size_t received;
static uint16_t last_seq;
static uint8_t response[RELAY_VALUE_MAX];
static size_t response_len;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t n)
{
	p[0] = n >> 8;
	p[1] = n;
}
static void put32(uint8_t *p, uint32_t n)
{
	p[0] = n >> 24;
	p[1] = n >> 16;
	p[2] = n >> 8;
	p[3] = n;
}
static void header(uint8_t kind, uint32_t sid, uint16_t seq, uint16_t len)
{
	memset(response, 0, RELAY_HEADER);
	response[0] = 1;
	response[1] = kind;
	put32(response + 2, sid);
	put16(response + 6, seq);
	put16(response + 10, len);
	response_len = RELAY_HEADER + len;
}
void relay_init(void)
{
	mutex = xSemaphoreCreateMutex();
	requests = xQueueCreate(1, sizeof(request_t));
	configASSERT(mutex && requests);
	session_counter = esp_random();
}
void relay_connected(bool value)
{
	xSemaphoreTake(mutex, portMAX_DELAY);
	connected = value;
	active = false;
	ending = true;
	busy = false;
	received = 0;
	response_len = 0;
	xQueueReset(requests);
	xSemaphoreGive(mutex);
}
size_t relay_status(uint8_t out[18])
{
	xSemaphoreTake(mutex, portMAX_DELAY);
	memset(out, 0, 18);
	out[0] = 1;
	out[1] = active ? 1 : 0;
	if (active) {
		out[2] = current.uid_len;
		out[3] = current.sak;
		put32(out + 4, session);
		memcpy(out + 8, current.uid, current.uid_len);
	}
	xSemaphoreGive(mutex);
	return 18;
}
size_t relay_response(uint8_t out[RELAY_VALUE_MAX])
{
	xSemaphoreTake(mutex, portMAX_DELAY);
	size_t n = response_len;
	memcpy(out, response, n);
	xSemaphoreGive(mutex);
	return n;
}
bool relay_write(const uint8_t *p, size_t n)
{
	if (n < RELAY_HEADER || p[0] != 1)
		return false;
	uint32_t sid = be32(p + 2);
	uint16_t seq = be16(p + 6), off = be16(p + 8), total = be16(p + 10);
	bool ok = false;
	xSemaphoreTake(mutex, portMAX_DELAY);
	if (!connected || !active || ending || sid != session)
		goto done;
	if (p[1] == 2) {
		if (n == RELAY_HEADER && off == 0 && total == 0) {
			ending = true;
			ok = true;
		}
		goto done;
	}
	if (p[1] != 1 || busy || total < 4 || total > RELAY_APDU_MAX || n == RELAY_HEADER ||
		off > total || n - RELAY_HEADER > (size_t)(total - off))
		goto done;
	if (off == 0) {
		if (seq == 0 || seq <= last_seq || received != 0)
			goto done;
		assembling.session = sid;
		assembling.seq = seq;
		assembling.length = total;
	}
	if (assembling.session != sid || assembling.seq != seq || assembling.length != total ||
		off != received)
		goto done;
	memcpy(assembling.data + off, p + RELAY_HEADER, n - RELAY_HEADER);
	received += n - RELAY_HEADER;
	ok = true;
	if (received == total) {
		if (xQueueSend(requests, &assembling, 0) != pdTRUE) {
			received = 0;
			ok = false;
			goto done;
		}
		busy = true;
		last_seq = seq;
		received = 0;
		header(0x10, sid, seq, 0);
	}
done:
	xSemaphoreGive(mutex);
	return ok;
}
/* Runs synchronously inside the NFC callback: no scans or RF-off commands
 * may run until this session has ended. No NimBLE calls from this task. */
bool relay_card(const nfc_card_t *card)
{
	xSemaphoreTake(mutex, portMAX_DELAY);
	if (!connected) {
		xSemaphoreGive(mutex);
		return false;
	}
	current = *card;
	session = ++session_counter;
	if (session == 0)
		session = ++session_counter;
	uint32_t sid = session;
	active = true;
	ending = false;
	busy = false;
	received = 0;
	last_seq = 0;
	response_len = 0;
	xQueueReset(requests);
	xSemaphoreGive(mutex);
	ESP_LOGI("homekey-reader", "NFC session %lu opened; sak=0x%02x", (unsigned long)sid, card->sak);
	int64_t deadline = esp_timer_get_time() + SESSION_TIMEOUT_US;
	for (;;) {
		xSemaphoreTake(mutex, portMAX_DELAY);
		bool stop = !active || !connected || ending || session != sid;
		xSemaphoreGive(mutex);
		if (stop || esp_timer_get_time() >= deadline)
			break;
		request_t req;
		if (xQueueReceive(requests, &req, pdMS_TO_TICKS(20)) != pdTRUE)
			continue;
		xSemaphoreTake(mutex, portMAX_DELAY);
		bool valid = active && connected && !ending && session == sid && req.session == sid;
		xSemaphoreGive(mutex);
		if (!valid)
			break;
		uint8_t data[RELAY_APDU_MAX];
		size_t length = 0;
		int error = pn532_exchange(card->target, req.data, req.length, data, sizeof(data), &length);
		xSemaphoreTake(mutex, portMAX_DELAY);
		if (active && connected && session == sid && !ending) {
			if (error == 0) {
				header(0x11, sid, req.seq, length);
				memcpy(response + RELAY_HEADER, data, length);
			} else {
				header(0x12, sid, req.seq, 1);
				response[RELAY_HEADER] = (uint8_t)error;
			}
			busy = false;
		}
		xSemaphoreGive(mutex);
		ESP_LOGI("homekey-reader", "APDU session=%lu seq=%u length=%u result=%d",
				 (unsigned long)sid, req.seq, (unsigned)length, error);
		if (error != 0)
			break;
	}
	xSemaphoreTake(mutex, portMAX_DELAY);
	if (session == sid) {
		active = false;
		busy = false;
		received = 0;
	}
	xSemaphoreGive(mutex);
	ESP_LOGI("homekey-reader", "NFC session %lu closed", (unsigned long)sid);
	return true;
}
