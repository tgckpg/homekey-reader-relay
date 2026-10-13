#include "mock.h"
#define ESP_GPIO_WAKEUP_GPIO_HIGH 1
#define ESP_SLEEP_WAKEUP_GPIO 7
int esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(uint64_t mask, int mode);
int esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int mode);
int esp_sleep_get_wakeup_cause(void);
void esp_deep_sleep_start(void);
