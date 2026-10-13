#include <stdio.h>
#include "../esp32/main/presence.c"
static int64_t now;
static int level, cause;
static unsigned armed, sleeps;
int64_t esp_timer_get_time(void) { return now; }
int gpio_config(const gpio_config_t *c)
{
    assert(c->pin_bit_mask == (1ULL << 5));
    assert(c->mode == GPIO_MODE_INPUT && c->pull_down_en);
    return ESP_OK;
}
int gpio_get_level(int pin) { assert(pin == 5); return level; }
int esp_sleep_get_wakeup_cause(void) { return cause; }
int esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(uint64_t mask, int mode)
{
    assert(mask == (1ULL << 5) && mode == ESP_GPIO_WAKEUP_GPIO_HIGH);
    ++armed;
    return ESP_OK;
}
int esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int mode)
{
    return esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(mask, mode);
}
void esp_deep_sleep_start(void) { assert(armed); ++sleeps; }
int main(void)
{
    presence_init();
#ifdef CONFIG_HOMEKEY_PRESENCE_ENABLED
    assert(armed == 1);
    assert(!presence_sleep_requested());
    now = 9999999;
    assert(!presence_sleep_requested());
    now = 10000000;
    assert(presence_sleep_requested());
    level = 1;
    assert(!presence_sleep_requested());
    level = 0;
    assert(!presence_sleep_requested());
    now += 999999;
    assert(!presence_sleep_requested());
    level = 1; /* An observed presence cancels the previous absence. */
    assert(!presence_sleep_requested());
    level = 0;
    assert(!presence_sleep_requested());
    now += 1000000;
    assert(presence_sleep_requested());
    /* New GPIO-wake boot skips cold-start grace. */
    now = 0;
    absent_since = -1;
    cause = ESP_SLEEP_WAKEUP_GPIO;
    presence_init();
    assert(!presence_sleep_requested());
    now = 1000000;
    assert(presence_sleep_requested());
    level = 1; /* Wake source was armed before a late return of presence. */
    presence_enter_sleep();
    assert(sleeps == 1);
#else
    now = 100000000;
    assert(!presence_sleep_requested());
    presence_enter_sleep();
    assert(armed == 0 && sleeps == 0);
#endif
    puts("Presence: boot grace, absence confirmation, return of presence, GPIO wake and disabled mode passed");
}
