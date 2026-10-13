#ifndef MOCK_GPIO_H
#define MOCK_GPIO_H
#include "mock.h"
#define GPIO_MODE_INPUT 1
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_down_en, pull_up_en, intr_type;
} gpio_config_t;
int gpio_config(const gpio_config_t *config);
int gpio_get_level(int pin);
int gpio_hold_en(int pin);
int gpio_hold_dis(int pin);
void gpio_deep_sleep_hold_en(void);
void gpio_deep_sleep_hold_dis(void);
#endif
