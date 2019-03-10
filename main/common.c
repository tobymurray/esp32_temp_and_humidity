#include "common.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

void pinModeOutput(uint8_t pin) {
	gpio_set_pull_mode(pin, GPIO_PULLUP_DISABLE);
	gpio_set_pull_mode(pin, GPIO_PULLDOWN_ENABLE);
	gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

unsigned long IRAM_ATTR millis() {
	return (unsigned long) (esp_timer_get_time() / 1000ULL);
}

unsigned long IRAM_ATTR micros() {
	return (unsigned long) (esp_timer_get_time());
}

void IRAM_ATTR delayMicroseconds(uint32_t us) {
	uint32_t m = micros();
	if (us) {
		uint32_t e = (m + us);
		if (m > e) { //overflow
			while (micros() > e) {
				NOP();
			}
		}
		while (micros() < e) {
			NOP();
		}
	}
}

void delay(uint32_t ms) {
	vTaskDelay(ms / portTICK_PERIOD_MS);
}
