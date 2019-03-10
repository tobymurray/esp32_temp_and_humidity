/*
 * Taken from: https://github.com/RobTillaart/Arduino/tree/master/libraries/DHTNEW and modified to suit my needs (e.g. native C instead of using Arduino)
 */

#include "dht.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "common.h"

#define LOW 0
#define HIGH 1

#define INPUT
#define OUTPUT

// For setting up critical sections (enableinterrupts and disableinterrupts not available)
// used to disable and interrupt interrupts
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

float _humOffset = 0.0;
float _tempOffset = 0.0;
uint32_t _lastRead = 0;
bool _disableIRQ = false;

static const char *DHT_TAG = "dht";

uint8_t _bits[5];  // buffer to receive data
int _readSensor(uint8_t pin);

/*
 * For some reason, gpio_config doesn't seem to work here. Interactions with the sensor start timing out.
 */
void pinModeInput(uint8_t pin) {
	gpio_set_pull_mode(pin, GPIO_PULLUP_ENABLE);
	gpio_set_pull_mode(pin, GPIO_PULLDOWN_DISABLE);
	gpio_set_direction(pin, GPIO_MODE_INPUT);
}

void IRAM_ATTR digitalWrite(uint8_t pin, uint8_t val) {
	if (val) {
		if (pin < 32) {
			GPIO.out_w1ts = ((uint32_t) 1 << pin);
		} else if (pin < 34) {
			GPIO.out1_w1ts.val = ((uint32_t) 1 << (pin - 32));
		}
	} else {
		if (pin < 32) {
			GPIO.out_w1tc = ((uint32_t) 1 << pin);
		} else if (pin < 34) {
			GPIO.out1_w1tc.val = ((uint32_t) 1 << (pin - 32));
		}
	}
}

int IRAM_ATTR digitalRead(uint8_t pin) {
	if (pin < 32) {
		return (GPIO.in >> pin) & 0x1;
	} else if (pin < 40) {
		return (GPIO.in1.val >> (pin - 32)) & 0x1;
	}
	return 0;
}


Reading readPin(uint8_t pin) {
	Reading reading;
	_lastRead = millis();

	// READ VALUES
	if (_disableIRQ) portENTER_CRITICAL_ISR(&mux);
	int readValue = _readSensor(pin);
	if (_disableIRQ) portEXIT_CRITICAL_ISR(&mux);

	if (readValue != DHTLIB_OK) {
		reading.humidity = DHTLIB_INVALID_VALUE;
		reading.temperature = DHTLIB_INVALID_VALUE;
		reading.status = readValue; // propagate error value
		return reading;
	}

	reading.humidity = (_bits[0] * 256 + _bits[1]) * 0.1;
	reading.temperature = ((_bits[2] & 0x7F) * 256 + _bits[3]) * 0.1;

	if (_bits[2] & 0x80) { // negative temperature
		reading.temperature = -reading.temperature;
	}

	reading.humidity += _humOffset;       // check overflow ???
	reading.temperature += _tempOffset;

	// TEST CHECKSUM
	uint8_t sum = _bits[0] + _bits[1] + _bits[2] + _bits[3];
	if (_bits[4] != sum) {
		ESP_LOGW(DHT_TAG, "Checksum failed!");
		reading.status = DHTLIB_ERROR_CHECKSUM;
	}

	reading.status = DHTLIB_OK;
	return reading;
}

int _readSensor(uint8_t pin) {
	// INIT BUFFERVAR TO RECEIVE DATA
	uint8_t mask = 128;
	uint8_t idx = 0;

	// EMPTY BUFFER
	for (uint8_t i = 0; i < 5; i++) {
		_bits[i] = 0;
	}

	// REQUEST SAMPLE

	pinModeOutput(pin);
	gpio_set_level(pin, LOW);
	delay(DHTLIB_DHT_WAKEUP);
	gpio_set_level(pin, HIGH);
	pinModeInput(pin);
	delayMicroseconds(40);

	// GET ACKNOWLEDGE or TIMEOUT
	uint16_t loopCnt = DHTLIB_TIMEOUT;
	while (gpio_get_level(pin) == LOW) {
		if (--loopCnt == 0) {
			ESP_LOGW(DHT_TAG, "Failed while waiting for acknowledgement (sensor didn't pull high)");
			return DHTLIB_ERROR_TIMEOUT;
		}
	}

	loopCnt = DHTLIB_TIMEOUT;
	while (gpio_get_level(pin) == HIGH) {
		if (--loopCnt == 0) {
			ESP_LOGW(DHT_TAG, "Failed while waiting for sensor response (sensor stayed high)");
			return DHTLIB_ERROR_TIMEOUT;
		}
	}

	// READ THE OUTPUT - 40 BITS => 5 BYTES
	for (uint8_t i = 40; i != 0; i--) {
		loopCnt = DHTLIB_TIMEOUT;
		while (gpio_get_level(pin) == LOW) {
			if (--loopCnt == 0) {
				ESP_LOGW(DHT_TAG, "Failed while reading data");
				return DHTLIB_ERROR_TIMEOUT;
			}
		}

		uint32_t t = micros();

		loopCnt = DHTLIB_TIMEOUT;
		while (gpio_get_level(pin) == HIGH) {
			if (--loopCnt == 0) {
				ESP_LOGW(DHT_TAG, "Timed out while waiting for sensor to pull low after data transmission");
				return DHTLIB_ERROR_TIMEOUT;
			}
		}

		if ((micros() - t) > 40) {
			_bits[idx] |= mask;
		}
		mask >>= 1;
		if (mask == 0)   // next byte?
				{
			mask = 128;
			idx++;
		}
	}

	return DHTLIB_OK;
}
