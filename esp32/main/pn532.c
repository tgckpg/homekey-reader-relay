#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "reader.h"
#include "relay.h"

#define PN532_UART UART_NUM_1
#define PN532_TX_GPIO 3
#define PN532_RX_GPIO 4
#define PN532_BAUD 115200
#define COMMAND_TIMEOUT_MS 1000
#define POLL_INTERVAL_MS 300

static const char *TAG = "pn532";
static const uint8_t ack[] = {0x00, 0x00, 0xff, 0x00, 0xff, 0x00};

static void pn532_uart_init(void)
{
	const uart_config_t config = {
		.baud_rate = PN532_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(PN532_UART, 1024, 0, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(PN532_UART, &config));
	ESP_ERROR_CHECK(uart_set_pin(PN532_UART, PN532_TX_GPIO, PN532_RX_GPIO,
								 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
	ESP_ERROR_CHECK(uart_flush_input(PN532_UART));
	ESP_LOGI(TAG, "UART ready: TX=GPIO%d RX=GPIO%d baud=%d",
			 PN532_TX_GPIO, PN532_RX_GPIO, PN532_BAUD);
}

static bool write_bytes(const uint8_t *data, size_t len)
{
	ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);
	return uart_write_bytes(PN532_UART, data, len) == (int)len &&
		   uart_wait_tx_done(PN532_UART, pdMS_TO_TICKS(100)) == ESP_OK;
}

/* One deadline for the entire command, including ACK and fragmented response. */
static bool read_exact(uint8_t *data, size_t len, int64_t deadline)
{
	size_t received = 0;
	while (received < len) {
		int64_t remaining = deadline - esp_timer_get_time();
		if (remaining <= 0) {
			return false;
		}
		TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
		if (ticks == 0) {
			ticks = 1;
		}
		int n = uart_read_bytes(PN532_UART, data + received, len - received, ticks);
		if (n < 0) {
			return false;
		}
		received += (size_t)n;
	}
	return true;
}

/* Returns 0 for ACK, positive for a normal frame, -1 for malformed/timeout.
 * Extended frames are unnecessary for these small commands and are rejected.
 */
static bool read_logged(uint8_t *buffer, size_t len,
						int64_t deadline, const char *stage)
{
	if (!read_exact(buffer, len, deadline)) {
		ESP_LOGW(TAG, "RX failed at %s: wanted %u bytes",
				 stage, (unsigned)len);
		return false;
	}

#ifdef FRAME_DEBUG
	ESP_LOGI(TAG, "RX %s (%u bytes)", stage, (unsigned)len);
	ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, len, ESP_LOG_INFO);
#endif
	return true;
}

static int read_frame(uint8_t *payload, size_t capacity, int64_t deadline)
{
	uint8_t prefix[3] = {0};
	do {
		prefix[0] = prefix[1];
		prefix[1] = prefix[2];
		if (!read_logged(&prefix[2], 1, deadline, "preamble byte")) {
			return -1;
		}
	} while (prefix[0] != 0 ||
			 prefix[1] != 0 ||
			 prefix[2] != 0xff);

	uint8_t lengths[2];
	if (!read_logged(lengths, sizeof(lengths), deadline, "LEN/LCS")) {
		return -1;
	}

	if (lengths[0] == 0 && lengths[1] == 0xff) {
		uint8_t postamble;
		if (!read_logged(&postamble, 1, deadline, "ACK postamble")) {
			return -1;
		}
		if (postamble != 0) {
			ESP_LOGW(TAG, "invalid ACK postamble");
			return -1;
		}
		return 0;
	}

	size_t len = lengths[0];
	if (len == 0 ||
		(uint8_t)(lengths[0] + lengths[1]) != 0 ||
		len > capacity) {
		ESP_LOGW(TAG, "invalid length: LEN=%u LCS=0x%02X capacity=%u",
				 (unsigned)len, lengths[1], (unsigned)capacity);
		return -1;
	}

	if (!read_logged(payload, len, deadline, "payload")) {
		return -1;
	}

	uint8_t tail[2];
	if (!read_logged(tail, sizeof(tail), deadline, "DCS/postamble")) {
		return -1;
	}
	if (tail[1] != 0) {
		ESP_LOGW(TAG, "invalid postamble: 0x%02X", tail[1]);
		return -1;
	}

	uint8_t checksum = tail[0];
	for (size_t i = 0; i < len; ++i) {
		checksum += payload[i];
	}
	if (checksum != 0) {
		ESP_LOGW(TAG, "invalid checksum: sum=0x%02X", checksum);
		return -1;
	}

	return (int)len;
}

/* Normal frame builder. cmd includes command code, excludes host TFI (D4). */
static int command_full(const uint8_t *cmd, size_t cmd_len, uint8_t *response,
						size_t capacity, unsigned timeout_ms)
{
	uint8_t frame[256];
	if (cmd_len == 0 || cmd_len > sizeof(frame) - 8 || cmd_len + 1 >= 255) {
		return -1;
	}

	size_t len = cmd_len + 1;
	frame[0] = 0;
	frame[1] = 0;
	frame[2] = 0xff;
	frame[3] = (uint8_t)len;
	frame[4] = (uint8_t)(0 - len);
	frame[5] = 0xd4;
	memcpy(frame + 6, cmd, cmd_len);

	uint8_t checksum = 0xd4;
	for (size_t i = 0; i < cmd_len; ++i) {
		checksum += cmd[i];
	}
	frame[6 + cmd_len] = (uint8_t)(0 - checksum);
	frame[7 + cmd_len] = 0;

#ifdef FRAME_DEBUG
	ESP_LOGI(TAG, "TX command 0x%02X (%u bytes)",
			 cmd[0], (unsigned)(cmd_len + 8));
	ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, cmd_len + 8, ESP_LOG_INFO);
#endif

	bool startup = cmd[0] == 0x02 || cmd[0] == 0x14;

	if (startup) {
		static const uint8_t wakeup[] = {0x55, 0x55, 0, 0, 0};
		uint8_t packet[sizeof(wakeup) + sizeof(frame)];
		size_t packet_len = sizeof(wakeup) + cmd_len + 8;

		memcpy(packet, wakeup, sizeof(wakeup));
		memcpy(packet + sizeof(wakeup), frame, cmd_len + 8);

		ESP_LOGI(TAG, "TX startup packet");
		ESP_LOG_BUFFER_HEX_LEVEL(
			TAG, packet, packet_len, ESP_LOG_INFO);

		if (!write_bytes(packet, packet_len)) {
			goto failed;
		}
	} else {
		if (!write_bytes(frame, cmd_len + 8)) {
			goto failed;
		}
	}

	int64_t deadline =
		esp_timer_get_time() + (int64_t)timeout_ms * 1000;
	int n;

	do {
		n = read_frame(response, capacity, deadline);
#ifdef FRAME_DEBUG
		if (n > 0) {
			ESP_LOGI(TAG, "RX response (%d bytes)", n);
			ESP_LOG_BUFFER_HEX_LEVEL(TAG, response, n, ESP_LOG_INFO);
		} else if (n == 0) {
			ESP_LOGI(TAG, "RX ACK");
		} else {
			ESP_LOGW(TAG, "RX failed or timed out: %d", n);
		}
#else
		if (n < 0) {
			ESP_LOGW(TAG, "RX failed or timed out: %d", n);
		}
#endif
	} while (n == 0); /* ACK is not the command response. */

	if (n >= 2 &&
		response[0] == 0xd5 &&
		response[1] == (uint8_t)(cmd[0] + 1)) {
		return n;
	}

failed:
	/* Host ACK aborts a pending PN532 command. Drain before the next command. */
	ESP_LOGW(TAG, "command 0x%02X failed or timed out", cmd[0]);

	ESP_LOGI(TAG, "TX abort ACK (%u bytes)", (unsigned)sizeof(ack));
	ESP_LOG_BUFFER_HEX_LEVEL(TAG, ack, sizeof(ack), ESP_LOG_INFO);
	(void)write_bytes(ack, sizeof(ack));

	vTaskDelay(pdMS_TO_TICKS(20));
	(void)uart_flush_input(PN532_UART);
	return -1;
}

static int command(const uint8_t *cmd, size_t length, uint8_t response[128])
{
	return command_full(cmd, length, response, 128, COMMAND_TIMEOUT_MS);
}

int pn532_exchange(uint8_t target, const uint8_t *apdu, size_t length, uint8_t *out,
				   size_t capacity, size_t *out_length)
{
	*out_length = 0;
	if (target == 0 || length < 4 || length > RELAY_APDU_MAX)
		return 1;
	uint8_t cmd[RELAY_APDU_MAX + 2] = {0x40, target};
	uint8_t response[256];
	memcpy(cmd + 2, apdu, length);
	int n = command_full(cmd, length + 2, response, sizeof(response), 3000);
	if (n < 3)
		return 2;
	/* Error code + MI/chaining flags. Chained PN532 responses are outside
	 * this STANDARD-auth transport's bounded APDU contract. */
	if (response[2] != 0) {
		ESP_LOGW(TAG, "InDataExchange status=0x%02x", response[2]);
		return 3;
	}
	if ((size_t)(n - 3) > capacity)
		return 4;
	memcpy(out, response + 3, n - 3);
	*out_length = n - 3;
	return 0;
}

static void release_rf(void)
{
	static const uint8_t rf_off[] = {0x32, 0x01, 0x00};
	uint8_t response[128];
	if (command(rf_off, sizeof(rf_off), response) != 2)
		ESP_LOGW(TAG, "RF-off failed");
}

static bool initialize_reader(void)
{
	static const uint8_t firmware[] = {0x02};
	static const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
	static const uint8_t retries[] = {0x32, 0x05, 0xff, 0x01, 0x00};
	uint8_t response[128];

	/* Discard stale input before starting this attempt. */
	ESP_ERROR_CHECK(uart_flush_input(PN532_UART));

	int n = command(firmware, sizeof(firmware), response);
	if (n != 6 || response[2] != 0x32) {
		ESP_LOGE(TAG, "invalid firmware response: length=%d", n);
		return false;
	}

	ESP_LOGI(TAG, "PN532 firmware %u.%u, support=0x%02X",
			 response[3], response[4], response[5]);

	vTaskDelay(pdMS_TO_TICKS(100));

	if (command(sam, sizeof(sam), response) != 2) {
		ESP_LOGE(TAG, "SAMConfiguration failed");
		return false;
	}

	vTaskDelay(pdMS_TO_TICKS(100));

	if (command(retries, sizeof(retries), response) != 2) {
		return false;
	}
	return true;
}

/* 1 = card, 0 = confirmed no target, -1 = transport/protocol failure. */
static int poll_card(nfc_card_t *card)
{
	static const uint8_t poll[] = {0x4a, 0x01, 0x00};
	uint8_t response[128];
	int n = command(poll, sizeof(poll), response);
	int result = -1;
	if (n == 3 && response[2] == 0) {
		result = 0;
	} else if (n >= 8 && response[2] == 1 &&
			   nfc_uid_valid(response[7]) && n >= 8 + response[7]) {
		card->target = response[3];
		card->sak = response[6];
		card->uid_len = response[7];
		memcpy(card->uid, response + 8, card->uid_len);
		result = 1;
	} else if (n >= 0) {
		ESP_LOGW(TAG, "invalid target response");
	}
	/* RF stays active through the callback's complete APDU session. */
	return result;
}

void start_polling(pn532_card_callback_t callback)
{
	pn532_uart_init();
	vTaskDelay(pdMS_TO_TICKS(1000));
	while (!initialize_reader()) {
		ESP_LOGE(TAG, "PN532 initialization failed; retrying in 2 seconds");
		vTaskDelay(pdMS_TO_TICKS(2000));
	}

	nfc_presence_t presence = {0};
	for (;;) {
		nfc_card_t card = {0};
		int result = poll_card(&card);
		bool was_present = presence.present;
		if (nfc_presence_update(&presence, result, &card)) {
			ESP_LOGI(TAG, "card detected, UID length=%u", card.uid_len);
			ESP_LOG_BUFFER_HEX_LEVEL(TAG, card.uid, card.uid_len, ESP_LOG_INFO);
			if (callback(&card)) {
				nfc_presence_accept(&presence, &card);
			}
		}
		if (was_present && !presence.present) {
			ESP_LOGI(TAG, "card removed");
		}
		release_rf();
		vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
	}
}
