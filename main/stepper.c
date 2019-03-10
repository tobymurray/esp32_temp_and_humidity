#define LOW 0
#define HIGH 1

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "common.h"
#include "esp_timer.h"

static const char *TAG = "stepper";

int steps[8][4] = {
	  {LOW, HIGH, HIGH, HIGH},
	  {LOW, LOW, HIGH, HIGH},
	  {HIGH, LOW, HIGH, HIGH},
	  {HIGH, LOW, LOW, HIGH},
	  {HIGH, HIGH, LOW, HIGH},
	  {HIGH, HIGH, LOW, LOW},
	  {HIGH, HIGH, HIGH, LOW},
	  {LOW, HIGH, HIGH, LOW}
	};

void set_up(int pins[]) {
	for (int i = 0; i < 4; i++) {
		ESP_LOGI(TAG, "Setting pin %d (%d) to output", i, pins[i]);
		pinModeOutput(pins[i]);
	}
}

void rotate(int pins[]) {
	ESP_LOGI(TAG, "Starting steps...");
	for (int i = 0; i < 1000; i++) {
		for (int step = 0; step < 8; step++) {
			for (int pin = 0; pin < 4; pin++) {
				delayMicroseconds(1000);
				gpio_set_level(pins[pin], steps[step][pin]);
			}
		}
	}
	ESP_LOGI(TAG, "...Steps done");
}
