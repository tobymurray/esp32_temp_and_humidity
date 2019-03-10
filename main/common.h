#ifndef common_h
#define common_h

#include <stdint.h>
#include "esp_attr.h"

#define NOP() asm volatile ("nop")

void pinModeOutput(uint8_t pin);
unsigned long IRAM_ATTR millis();
unsigned long IRAM_ATTR micros();
void IRAM_ATTR delayMicroseconds(uint32_t us);
void delay(uint32_t ms);
void IRAM_ATTR delayMicroseconds(uint32_t us);

#endif

// END OF FILE
