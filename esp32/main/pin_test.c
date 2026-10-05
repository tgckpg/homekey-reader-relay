#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TEST_UART UART_NUM_1
#define TX_PIN 4
#define RX_PIN 3

static const char *TAG = "uart-loopback";

void app_main(void)
{
	const uart_config_t config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	ESP_ERROR_CHECK(uart_param_config(TEST_UART, &config));
	ESP_ERROR_CHECK(uart_set_pin(
		TEST_UART, TX_PIN, RX_PIN,
		UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
	ESP_ERROR_CHECK(uart_driver_install(
		TEST_UART, 256, 0, 0, NULL, 0));

	const uint8_t tx[] = {
		0x55, 0xaa, 0x00, 0xff, 0x02, 0xfe, 0xd4, 0x2a
	};

	for (;;) {
		uint8_t rx[sizeof(tx)] = {0};
		size_t received = 0;

		ESP_ERROR_CHECK(uart_flush_input(TEST_UART));

		int written = uart_write_bytes(TEST_UART, tx, sizeof(tx));
		ESP_LOGI(TAG, "TX: %d bytes", written);
		ESP_LOG_BUFFER_HEX_LEVEL(TAG, tx, sizeof(tx), ESP_LOG_INFO);

		/* Accumulate reads: UART may return fewer bytes than requested. */
		while (received < sizeof(rx)) {
			int n = uart_read_bytes(
				TEST_UART,
				rx + received,
				sizeof(rx) - received,
				pdMS_TO_TICKS(500));

			if (n <= 0) {
				ESP_LOGW(TAG, "read stopped: %d", n);
				break;
			}
			received += (size_t)n;
		}

		ESP_LOGI(TAG, "RX: %u bytes", (unsigned)received);
		if (received > 0) {
			ESP_LOG_BUFFER_HEX_LEVEL(
				TAG, rx, received, ESP_LOG_INFO);
		}

		if (written == sizeof(tx) &&
			received == sizeof(tx) &&
			memcmp(tx, rx, sizeof(tx)) == 0) {
			ESP_LOGI(TAG, "PASS: RX = TX");
		} else {
			ESP_LOGE(TAG, "FAIL: RX != TX");
		}

		vTaskDelay(pdMS_TO_TICKS(2000));
	}
}