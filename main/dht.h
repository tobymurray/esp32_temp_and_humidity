#ifndef dhtnew_h
#define dhtnew_h

#define DHTLIB_OK                0
#define DHTLIB_ERROR_CHECKSUM   -1
#define DHTLIB_ERROR_TIMEOUT    -2
#define DHTLIB_INVALID_VALUE    -999

#define DHTLIB_DHT_WAKEUP       10

// max timeout is 100usec.
// For a 16Mhz proc that is max 1600 clock cycles
// loops using TIMEOUT use at least 4 clock cycli
// so 100 us takes max 400 loops
// so by dividing F_CPU by 40000 we "fail" as fast as possible
#define DHTLIB_TIMEOUT (240000000L/40000)

#include <stdint.h>

typedef struct reading {
	float humidity;
	float temperature;
	int status;
} Reading;

Reading readPin(uint8_t pin);

#endif

// END OF FILE
