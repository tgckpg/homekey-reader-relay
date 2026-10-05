#ifndef MOCK_H
#define MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef unsigned TickType_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NO_FREE_PAGES 1
#define ESP_ERR_NVS_NEW_VERSION_FOUND 2
#define ESP_LOG_INFO 1
#define ESP_LOG_DEBUG 2
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
static inline void mock_log(const char *tag, const char *fmt, ...) {(void)tag;(void)fmt;}
#define ESP_LOGI mock_log
#define ESP_LOGW mock_log
#define ESP_LOGE mock_log
#define ESP_LOG_BUFFER_HEX_LEVEL(t,d,n,l) do {(void)(t);(void)(d);(void)(n);(void)(l);} while(0)
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define pdTRUE 1
#define UART_NUM_1 1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
typedef struct {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;} uart_config_t;
int uart_driver_install(int,int,int,int,void*,int);
int uart_param_config(int,const uart_config_t*);
int uart_set_pin(int,int,int,int,int);
int uart_flush_input(int);
int uart_write_bytes(int,const void*,size_t);
int uart_wait_tx_done(int,TickType_t);
int uart_read_bytes(int,void*,size_t,TickType_t);
int64_t esp_timer_get_time(void);
void vTaskDelay(TickType_t);
uint32_t esp_random(void);
int nvs_flash_init(void);
int nvs_flash_erase(void);
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned,unsigned);
int xQueueSend(QueueHandle_t,const void*,TickType_t);
int xQueueReceive(QueueHandle_t,void*,TickType_t);
#endif
