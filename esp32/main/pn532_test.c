#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PN532_UART	  UART_NUM_1
#define PN532_TX_GPIO   3
#define PN532_RX_GPIO   4
#define PN532_BAUD	  115200

static const char *TAG = "pn532";

static void dump_hex(const char *prefix, const uint8_t *buf, size_t len)
{
	printf("%s", prefix);

	for (size_t i = 0; i < len; i++) {
		printf("%02X ", buf[i]);
	}

	printf("\n");
}

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

	ESP_ERROR_CHECK(uart_driver_install(
		PN532_UART,
		1024,
		0,
		0,
		NULL,
		0
	));

	ESP_ERROR_CHECK(uart_param_config(
		PN532_UART,
		&config
	));

	ESP_ERROR_CHECK(uart_set_pin(
		PN532_UART,
		PN532_TX_GPIO,
		PN532_RX_GPIO,
		UART_PIN_NO_CHANGE,
		UART_PIN_NO_CHANGE
	));

	uart_flush_input(PN532_UART);

	ESP_LOGI(
		TAG,
		"UART ready: TX=GPIO%d RX=GPIO%d baud=%d",
		PN532_TX_GPIO,
		PN532_RX_GPIO,
		PN532_BAUD
	);
}

static int pn532_read(uint8_t *buf, size_t size, int timeout_ms)
{
	return uart_read_bytes(
		PN532_UART,
		buf,
		size,
		pdMS_TO_TICKS(timeout_ms)
	);
}

static void pn532_write(const uint8_t *buf, size_t len)
{
	dump_hex("TX: ", buf, len);

	uart_write_bytes(
		PN532_UART,
		buf,
		len
	);

	uart_wait_tx_done(
		PN532_UART,
		pdMS_TO_TICKS(100)
	);
}

static void pn532_wakeup(void)
{
	/*
	 * PN532 HSU wake-up preamble.
	 *
	 * The PN532 manual specifies 55 55 followed by the
	 * normal synchronization sequence for waking from
	 * LowVbat mode.
	 */
	static const uint8_t wakeup[] = {
		0x55, 0x55,
		0x00, 0x00, 0x00, 0x00, 0x00,
		0xFF
	};

	ESP_LOGI(TAG, "sending wakeup");

	pn532_write(wakeup, sizeof(wakeup));

	vTaskDelay(pdMS_TO_TICKS(10));

	uart_flush_input(PN532_UART);
}

static void pn532_get_firmware_version(void)
{
	static const uint8_t cmd[] = {
		/* HSU wakeup */
		0x55, 0x55,
		0x00, 0x00, 0x00, 0x00, 0x00,

		/* PN532 frame */
		0xFF,
		0x02, 0xFE,
		0xD4, 0x02,
		0x2A,
		0x00
	};

	uint8_t buf[64];

	uart_flush_input(PN532_UART);

	ESP_LOGI(TAG, "GetFirmwareVersion");
	pn532_write(cmd, sizeof(cmd));

	int len = pn532_read(buf, sizeof(buf), 1000);

	if (len <= 0) {
		ESP_LOGE(TAG, "no response from PN532");
		return;
	}

	dump_hex("RX: ", buf, len);

	len = pn532_read(buf, sizeof(buf), 500);

	if (len > 0) {
		dump_hex("RX: ", buf, len);
	}
}

static int pn532_read_response(uint8_t *buf, size_t size, int timeout_ms)
{
	int len = pn532_read(buf, size, timeout_ms);

	if (len <= 0) {
		return len;
	}

	dump_hex("RX: ", buf, len);
	return len;
}

static void pn532_poll_card(void)
{
	/*
	 * InListPassiveTarget
	 *
	 * D4 4A 01 00
	 *	│  │  └── BrTy = 0x00: ISO14443A @ 106 kbps
	 *	│  └───── MaxTg = 1
	 *	└──────── command
	 *
	 * Complete PN532 frame:
	 *
	 * 00 00 FF 04 FC D4 4A 01 00 E1 00
	 */
	static const uint8_t cmd[] = {
		0x00, 0x00, 0xFF,
		0x04, 0xFC,
		0xD4, 0x4A, 0x01, 0x00,
		0xE1,
		0x00
	};

	uint8_t buf[128];

	uart_flush_input(PN532_UART);

	pn532_write(cmd, sizeof(cmd));

	int len = pn532_read_response(
		buf,
		sizeof(buf),
		1000
	);

	if (len <= 0) {
		/*
		 * No card is normal.
		 */
		return;
	}

	/*
	 * ACK:
	 *
	 * 00 00 FF 00 FF 00
	 *
	 * Often ACK + response arrive in one UART read.
	 *
	 * Find the response frame:
	 *
	 * 00 00 FF LEN LCS D5 4B ...
	 */
	int frame = -1;

	for (int i = 0; i + 6 < len; i++) {
		if (
			buf[i + 0] == 0x00 &&
			buf[i + 1] == 0x00 &&
			buf[i + 2] == 0xFF &&
			buf[i + 5] == 0xD5 &&
			buf[i + 6] == 0x4B
		) {
			frame = i;
			break;
		}
	}

	/*
	 * Response may have arrived separately from ACK.
	 */
	if (frame < 0) {
		len = pn532_read_response(
			buf,
			sizeof(buf),
			1000
		);

		if (len <= 0) {
			return;
		}

		for (int i = 0; i + 6 < len; i++) {
			if (
				buf[i + 0] == 0x00 &&
				buf[i + 1] == 0x00 &&
				buf[i + 2] == 0xFF &&
				buf[i + 5] == 0xD5 &&
				buf[i + 6] == 0x4B
			) {
				frame = i;
				break;
			}
		}
	}

	if (frame < 0) {
		ESP_LOGW(TAG, "no InListPassiveTarget response found");
		return;
	}

	/*
	 * Payload after D5 4B:
	 *
	 * NbTg
	 *
	 * If NbTg > 0:
	 *
	 * Tg
	 * SENS_RES[2]
	 * SEL_RES
	 * NFCIDLength
	 * NFCID[]
	 */
	int p = frame + 7;

	if (p >= len) {
		return;
	}

	uint8_t targets = buf[p++];

	if (targets == 0) {
		return;
	}

	if (p + 5 > len) {
		ESP_LOGW(TAG, "short target response");
		return;
	}

	uint8_t tg = buf[p++];

	uint8_t sens_res_0 = buf[p++];
	uint8_t sens_res_1 = buf[p++];

	uint8_t sel_res = buf[p++];

	uint8_t uid_len = buf[p++];

	if (uid_len == 0 || uid_len > 10 || p + uid_len > len) {
		ESP_LOGW(TAG, "invalid UID length: %u", uid_len);
		return;
	}

	ESP_LOGI(
		TAG,
		"card: Tg=%u ATQA=%02X%02X SAK=%02X UID len=%u",
		tg,
		sens_res_0,
		sens_res_1,
		sel_res,
		uid_len
	);

	printf("UID: ");

	for (int i = 0; i < uid_len; i++) {
		if (i) {
			printf(":");
		}

		printf("%02X", buf[p + i]);
	}

	printf("\n");
}

static bool pn532_sam_config(void)
{
	/*
	 * HSU wake-up + SAMConfiguration
	 *
	 * D4 14 01 14 01
	 *	   │  │  └── IRQ enabled
	 *	   │  └───── timeout
	 *	   └──────── normal mode
	 *
	 * DCS:
	 * D4 + 14 + 01 + 14 + 01 = FE
	 * checksum = 02
	 */
	static const uint8_t cmd[] = {
		/* HSU wake-up */
		0x55, 0x55,
		0x00, 0x00, 0x00, 0x00, 0x00,

		/* PN532 frame */
		0xFF,
		0x05, 0xFB,
		0xD4, 0x14, 0x01, 0x14, 0x01,
		0x02,
		0x00
	};

	uint8_t buf[64];

	uart_flush_input(PN532_UART);

	ESP_LOGI(TAG, "SAMConfiguration");
	pn532_write(cmd, sizeof(cmd));

	int len = pn532_read(buf, sizeof(buf), 1000);

	if (len <= 0) {
		ESP_LOGE(TAG, "SAMConfiguration: no response");
		return false;
	}

	dump_hex("RX: ", buf, len);

	/*
	 * Expected somewhere in the response:
	 *
	 * ACK:
	 * 00 00 FF 00 FF 00
	 *
	 * response:
	 * 00 00 FF 02 FE D5 15 16 00
	 */

	return true;
}

void app_main(void)
{
	ESP_LOGI(TAG, "PN532 NFC reader");

	pn532_uart_init();

	vTaskDelay(pdMS_TO_TICKS(1000));

	pn532_get_firmware_version();

	vTaskDelay(pdMS_TO_TICKS(100));

	if (!pn532_sam_config()) {
		ESP_LOGE(TAG, "PN532 initialization failed");
		return;
	}

	vTaskDelay(pdMS_TO_TICKS(100));

	while (1) {
		pn532_poll_card();
		vTaskDelay(pdMS_TO_TICKS(300));
	}
}
