#include <stdio.h>
#include "../esp32/main/pn532.c"
static uint8_t stream[1024];
static size_t total, pos, chunk = 1;
static int64_t now;
static unsigned writes;
static uint8_t last_tx[300];
static size_t last_tx_len;
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t t) { now += t * 1000; }
int uart_read_bytes(int u, void *dst, size_t len, TickType_t t)
{
	(void)u;
	now += 100;
	if (pos == total) {
		now += t * 1000;
		return 0;
	}
	size_t n = total - pos;
	if (n > len)
		n = len;
	if (n > chunk)
		n = chunk;
	memcpy(dst, stream + pos, n);
	pos += n;
	return (int)n;
}
int uart_write_bytes(int u, const void *d, size_t n)
{
	(void)u;
	assert(n <= sizeof(last_tx));
	memcpy(last_tx, d, n);
	last_tx_len = n;
	++writes;
	return (int)n;
}
int uart_wait_tx_done(int u, TickType_t t)
{
	(void)u;
	(void)t;
	return 0;
}
int uart_flush_input(int u)
{
	(void)u;
	pos = total;
	return 0;
}
int uart_driver_install(int a, int b, int c, int d, void *e, int f)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	return 0;
}
int uart_param_config(int u, const uart_config_t *c)
{
	(void)u;
	(void)c;
	return 0;
}
int uart_set_pin(int a, int b, int c, int d, int e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return 0;
}
static void reset(void)
{
	total = pos = 0;
	now = 0;
	writes = 0;
}
static void append(const uint8_t *d, size_t n)
{
	memcpy(stream + total, d, n);
	total += n;
}
static void response(const uint8_t *d, uint8_t n)
{
	uint8_t h[] = {0, 0, 255, n, (uint8_t)(0 - n)};
	append(ack, sizeof(ack));
	append(h, 5);
	append(d, n);
	uint8_t sum = 0;
	for (unsigned i = 0; i < n; i++)
		sum += d[i];
	uint8_t tail[] = {(uint8_t)(0 - sum), 0};
	append(tail, 2);
}
int main(void)
{
	uint8_t out[128];
	const uint8_t fw[] = {0xd5, 3, 0x32, 1, 6, 7}, cmd[] = {2};
	for (chunk = 1; chunk <= 64; chunk *= 2) {
		reset();
		response(fw, 6);
		assert(command(cmd, 1, out) == 6);
		assert(memcmp(out, fw, 6) == 0);
		assert(writes == 1);
		assert(last_tx_len == 14 && memcmp(last_tx, "\x55\x55\0\0\0", 5) == 0);
	}
	reset();
	response(fw, 6);
	stream[total - 2] ^= 1;
	assert(command(cmd, 1, out) == -1);
	assert(writes == 2);
	reset();
	response(fw, 6);
	stream[sizeof(ack) + 4] ^= 1;
	assert(command(cmd, 1, out) == -1);
	reset();
	response(fw, 6);
	--total;
	assert(command(cmd, 1, out) == -1);
	reset();
	append(ack, sizeof(ack));
	assert(command(cmd, 1, out) == -1);
	const uint8_t rf[] = {0xd5, 0x33};
	const unsigned lens[] = {4, 7, 10};
	for (unsigned k = 0; k < 3; k++) {
		uint8_t target[18] = {0xd5, 0x4b, 1, 1, 0, 4, 0};
		target[7] = lens[k];
		for (unsigned j = 0; j < lens[k]; j++)
			target[8 + j] = j + 1;
		reset();
		response(target, 8 + lens[k]);
		response(rf, 2);
		nfc_card_t card = {0};
		assert(poll_card(&card) == 1);
		assert(card.uid_len == lens[k]);
		assert(memcmp(card.uid, target + 8, lens[k]) == 0);
	}
	uint8_t absent[] = {0xd5, 0x4b, 0};
	reset();
	response(absent, 3);
	response(rf, 2);
	nfc_card_t card = {0};
	assert(poll_card(&card) == 0);
	uint8_t short_uid[] = {0xd5, 0x4b, 1, 1, 0, 4, 0, 10, 1};
	reset();
	response(short_uid, sizeof(short_uid));
	response(rf, 2);
	assert(poll_card(&card) == -1);
	reset();
	response(absent, 3);
	stream[total - 2] ^= 1;
	assert(poll_card(&card) == -1);
	uint8_t apdu[114] = {0x80, 0x80, 0, 1}, reply[] = {0xd5, 0x41, 0, 0x90, 0};
	size_t out_length = 0;
	reset();
	response(reply, sizeof(reply));
	assert(pn532_exchange(1, apdu, sizeof(apdu), out, sizeof(out), &out_length) == 0);
	assert(out_length == 2 && out[0] == 0x90);
	assert(last_tx_len == sizeof(apdu) + 10);
	assert(last_tx[5] == 0xd4 && last_tx[6] == 0x40 && last_tx[7] == 1);
	assert(memcmp(last_tx + 8, apdu, sizeof(apdu)) == 0);
	reset();
	reply[2] = 1;
	response(reply, sizeof(reply));
	assert(pn532_exchange(1, apdu, sizeof(apdu), out, sizeof(out), &out_length) == 3);
	reset();
	reply[2] = 0x40;
	response(reply, sizeof(reply));
	assert(pn532_exchange(1, apdu, sizeof(apdu), out, sizeof(out), &out_length) == 3);
	assert(pn532_exchange(1, apdu, RELAY_APDU_MAX + 1, out, sizeof(out), &out_length) == 1);
	puts("PN532: startup prefix preserved; APDU >64 bytes; status/chaining rejection; fragmented "
		 "frames, checksums, timeout and UID parsing passed");
}
