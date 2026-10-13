#include "presence.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#ifdef CONFIG_HOMEKEY_PRESENCE_ENABLED
static int64_t boot_deadline;
static int64_t absent_since = -1;
#endif

void presence_init(void)
{
#ifdef CONFIG_HOMEKEY_PRESENCE_ENABLED
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << CONFIG_HOMEKEY_PRESENCE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input));
    /* Arm before any shutdown work. Level wake also covers presence returning
     * between the final check and entry into deep sleep. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        input.pin_bit_mask, ESP_GPIO_WAKEUP_GPIO_HIGH));
#else
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(
        input.pin_bit_mask, ESP_GPIO_WAKEUP_GPIO_HIGH));
#endif
    boot_deadline = esp_timer_get_time();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
	bool gpio_wakeup = (esp_sleep_get_wakeup_causes() &
			(1U << ESP_SLEEP_WAKEUP_GPIO)) != 0;
#else
	bool gpio_wakeup = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
#endif
	if (!gpio_wakeup)
        boot_deadline += (int64_t)CONFIG_HOMEKEY_PRESENCE_BOOT_GRACE_MS * 1000;
    ESP_LOGI("presence", "LD2410S OT2=GPIO%d (HIGH=present); deep sleep enabled",
             CONFIG_HOMEKEY_PRESENCE_GPIO);
#endif
}

bool presence_sleep_requested(void)
{
#ifdef CONFIG_HOMEKEY_PRESENCE_ENABLED
    int64_t now = esp_timer_get_time();
	int level = gpio_get_level(CONFIG_HOMEKEY_PRESENCE_GPIO);
	// ESP_LOGI("presence", "GPIO%d reads %d", CONFIG_HOMEKEY_PRESENCE_GPIO, level);
	if (level) {
        absent_since = -1;
        return false;
    }
    if (absent_since < 0)
        absent_since = now;
    return now >= boot_deadline &&
           now - absent_since >= (int64_t)CONFIG_HOMEKEY_PRESENCE_ABSENCE_MS * 1000;
#else
    return false;
#endif
}

void presence_enter_sleep(void)
{
#ifdef CONFIG_HOMEKEY_PRESENCE_ENABLED
    ESP_LOGI("presence", "Absent: PN532 asleep, BLE stopped; ESP32 deep sleep");
    esp_deep_sleep_start();
#endif
}
