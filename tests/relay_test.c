#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../esp32/main/relay.c"
static request_t queued;
static bool have_queued;
static int phase, scenario, exchanges;
static int64_t clock_us;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t t)
{
	(void)m;
	(void)t;
	return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t m)
{
	(void)m;
	return pdTRUE;
}
QueueHandle_t xQueueCreate(unsigned n, unsigned size)
{
	assert(n == 1 && size == sizeof(request_t));
	return (void *)1;
}
int xQueueReset(QueueHandle_t q)
{
	(void)q;
	have_queued = false;
	return pdTRUE;
}
int xQueueSend(QueueHandle_t q, const void *p, TickType_t t)
{
	(void)q;
	(void)t;
	if (have_queued)
		return 0;
	queued = *(const request_t *)p;
	have_queued = true;
	return pdTRUE;
}
uint32_t esp_random(void) { return 100; }
int64_t esp_timer_get_time(void) { return clock_us; }
static size_t make_request(uint8_t out[32], uint8_t kind, uint32_t sid, uint16_t seq, uint16_t off,
						   uint16_t len, const uint8_t *d, size_t n)
{
	memset(out, 0, RELAY_HEADER);
	out[0] = 1;
	out[1] = kind;
	put32(out + 2, sid);
	put16(out + 6, seq);
	put16(out + 8, off);
	put16(out + 10, len);
	if (n)
		memcpy(out + 12, d, n);
	return 12 + n;
}
int xQueueReceive(QueueHandle_t q, void *p, TickType_t t)
{
	(void)q;
	clock_us += t * 1000;
	if (phase == 0) {
		uint8_t s[18], b[32], d[] = {0, 0xa4, 4, 0};
		relay_status(s);
		assert(s[1] == 1 && be32(s + 4) == session && s[3] == 0x20);
		assert(!relay_write(b, make_request(b, 1, session - 1, 1, 0, 4, d, 4)));
		assert(!relay_write(b, make_request(b, 1, session, 1, 2, 4, d + 2, 2)));
		assert(relay_write(b, make_request(b, 1, session, 1, 0, 4, d, 2)));
		assert(!relay_write(b, make_request(b, 1, session, 1, 0, 4, d, 2))); // Duplicate offset.
		assert(!relay_write(b, make_request(b, 1, session, 2, 2, 4, d + 2, 2))); // Wrong sequence.
		assert(relay_write(b, make_request(b, 1, session, 1, 2, 4, d + 2, 2)));
		assert(!relay_write(b, make_request(b, 1, session, 2, 0, 4, d, 4))); // Busy.
		uint8_t r[RELAY_VALUE_MAX];
		assert(relay_response(r) == 12 && r[1] == 0x10);
		phase = 1;
	} else if (phase == 1) {
		uint8_t r[RELAY_VALUE_MAX], b[32], d[] = {0x80, 0x3c, 0, 0};
		size_t n = relay_response(r);
		assert(n == 14 && r[1] == 0x11 && be16(r + 6) == 1 && r[12] == 0x90 && r[13] == 0);
		assert(!relay_write(b, make_request(b, 1, session, 1, 0, 4, d, 4))); // Replay.
		if (scenario == 1) {
			relay_connected(false);
		} else if (scenario == 2) {
			clock_us += SESSION_TIMEOUT_US;
		} else {
			assert(relay_write(b, make_request(b, 2, session, 0, 0, 0, NULL, 0)));
		}
		phase = 2;
	}
	if (!have_queued)
		return 0;
	*(request_t *)p = queued;
	have_queued = false;
	return pdTRUE;
}
int pn532_exchange(uint8_t target, const uint8_t *apdu, size_t len, uint8_t *out, size_t capacity,
				   size_t *n)
{
	assert(target == 1 && len == 4 && apdu[1] == 0xa4 && capacity >= 2);
	exchanges++;
	out[0] = 0x90;
	out[1] = 0;
	*n = 2;
	return 0;
}
int main(void)
{
	relay_init();
	nfc_card_t card = {.uid = {1, 2, 3, 4}, .uid_len = 4, .target = 1, .sak = 0x20};
	assert(!relay_card(&card));
	uint8_t group[8], b[32], gid[8] = {1,2,3,4,5,6,7,8};
	assert(!relay_ecp_group(group));
	assert(!relay_write(b, make_request(b, 3, 0, 0, 0, 8, gid, 8)));
	relay_connected(true);
	assert(!relay_write(b, make_request(b, 3, 1, 0, 0, 8, gid, 8)));
	assert(!relay_write(b, make_request(b, 3, 0, 0, 0, 7, gid, 7)));
	assert(relay_write(b, make_request(b, 3, 0, 0, 0, 8, gid, 8)));
	assert(relay_ecp_group(group) && memcmp(group, gid, 8) == 0);
	active = true;
	assert(relay_write(b, make_request(b, 3, 0, 0, 0, 8, gid, 8)));
	assert(active); /* Configuration never interrupts the APDU session. */
	active = false;
	assert(relay_write(b, make_request(b, 3, 0, 0, 0, 0, NULL, 0)));
	assert(!relay_ecp_group(group));
	assert(relay_write(b, make_request(b, 3, 0, 0, 0, 8, gid, 8)));
	relay_connected(false);
	assert(!relay_ecp_group(group));
	for (scenario = 0; scenario < 3; scenario++) {
		relay_connected(true);
		phase = exchanges = 0;
		clock_us = 0;
		assert(relay_card(&card));
		assert(exchanges == 1 && !active);
		uint8_t s[18];
		relay_status(s);
		assert(s[1] == 0);
	}
	puts("Relay: fragment order, busy rejection, stale session, replay, response, release, "
		 "disconnect and timeout passed");
}
