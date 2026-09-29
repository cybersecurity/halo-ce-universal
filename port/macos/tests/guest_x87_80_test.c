#include "../guest_x87_80.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static void roundtrip(double input)
{
	uint8_t encoded[10];
	double output = 1.0;
	assert(mac_guest_x87_80_store(encoded, input) == 0);
	assert(mac_guest_x87_80_load(encoded, 0, &output) == 0);
	if (isnan(input))
		assert(isnan(output));
	else
		assert(memcmp(&input, &output, sizeof(input)) == 0);
}

int main(void)
{
	uint8_t raw[10] = {0};
	double output;

	roundtrip(0.0);
	roundtrip(-0.0);
	roundtrip(1.0);
	roundtrip(-2.5);
	roundtrip(0x1.fffffffffffffp+1023);
	roundtrip(0x1p-1022);
	roundtrip(0x0.0000000000001p-1022);
	roundtrip(INFINITY);
	roundtrip(-INFINITY);
	roundtrip(NAN);

	/* Decode canonical values written independently of the encoder. */
	memset(raw, 0, sizeof(raw));
	raw[7] = 0x80;
	raw[8] = 0xFF;
	raw[9] = 0x3F; /* +1.0 */
	assert(mac_guest_x87_80_load(raw, 0, &output) == 0 && output == 1.0);
	memset(raw, 0, sizeof(raw));
	raw[7] = 0x80;
	raw[8] = 0xCD;
	raw[9] = 0x3B; /* binary64's smallest subnormal, encoded as binary80 */
	assert(mac_guest_x87_80_load(raw, 0, &output) == 0);
	assert(output == 0x0.0000000000001p-1022);

	/* A finite binary80 halfway value rounds to nearest-even. */
	memset(raw, 0, sizeof(raw));
	raw[7] = 0x80;
	raw[8] = 0xFF;
	raw[9] = 0x3F;
	raw[1] = 0x04; /* 1 + 2^-53, tie rounds to even 1.0 */
	assert(mac_guest_x87_80_load(raw, 0, &output) == 0 && output == 1.0);
	assert(mac_guest_x87_80_load(raw, 2, &output) == 0 && output == nextafter(1.0, INFINITY));
	assert(mac_guest_x87_80_load(raw, 1, &output) == 0 && output == 1.0);
	assert(mac_guest_x87_80_load(raw, 3, &output) == 0 && output == 1.0);

	/* A halfway value between zero and the least subnormal follows guest RC. */
	memset(raw, 0, sizeof(raw));
	raw[7] = 0x80;
	raw[8] = 0xCC;
	raw[9] = 0x3B; /* 2^-1075 */
	assert(mac_guest_x87_80_load(raw, 0, &output) == 0 && output == 0.0);
	assert(mac_guest_x87_80_load(raw, 2, &output) == 0 && output == 0x0.0000000000001p-1022);

	/* Directed conversion handles finite overflow and tiny extended inputs. */
	memset(raw, 0, sizeof(raw));
	raw[7] = 0x80;
	raw[8] = 0xFF;
	raw[9] = 0x43; /* +2^1024: exponent bias 16383 + 1024 */
	assert(mac_guest_x87_80_load(raw, 0, &output) == 0 && isinf(output) && output > 0);
	assert(mac_guest_x87_80_load(raw, 3, &output) == 0 && output == 0x1.fffffffffffffp+1023);
	raw[9] |= 0x80;
	assert(mac_guest_x87_80_load(raw, 1, &output) == 0 && isinf(output) && output < 0);

	memset(raw, 0, sizeof(raw));
	raw[0] = 1; /* positive binary80 subnormal */
	assert(mac_guest_x87_80_load(raw, 0, &output) == 0 && output == 0.0);
	assert(mac_guest_x87_80_load(raw, 2, &output) == 0 && output == 0x0.0000000000001p-1022);

	/* Noncanonical unnormal values fail closed. */
	memset(raw, 0, sizeof(raw));
	raw[7] = 0x40;
	raw[8] = 1;
	assert(mac_guest_x87_80_load(raw, 0, &output) == -1);

	return 0;
}
