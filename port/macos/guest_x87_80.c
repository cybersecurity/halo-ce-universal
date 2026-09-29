#include "guest_x87_80.h"

#include <string.h>

static uint64_t read_le64(const uint8_t *bytes)
{
	uint64_t value = 0;
	for (unsigned i = 0; i < 8; ++i)
		value |= (uint64_t)bytes[i] << (i * 8);
	return value;
}

static uint16_t read_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
	for (unsigned i = 0; i < 8; ++i)
		bytes[i] = (uint8_t)(value >> (i * 8));
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static int double_from_bits(uint64_t bits, double *out)
{
	memcpy(out, &bits, sizeof(bits));
	return 0;
}

static uint64_t round_right(uint64_t value, unsigned shift, unsigned sign,
	unsigned rounding_mode, int *inexact)
{
	uint64_t quotient, remainder;
	int increment = 0;

	if (!shift)
	{
		*inexact = 0;
		return value;
	}
	if (shift < 64)
	{
		quotient = value >> shift;
		remainder = value & ((UINT64_C(1) << shift) - 1);
		*inexact = remainder != 0;
		if (rounding_mode == 0)
		{
			uint64_t halfway = UINT64_C(1) << (shift - 1);
			increment = remainder > halfway || (remainder == halfway && (quotient & 1));
		}
	}
	else if (shift == 64)
	{
		quotient = 0;
		remainder = value;
		*inexact = remainder != 0;
		if (rounding_mode == 0)
			increment = remainder > (UINT64_C(1) << 63);
	}
	else
	{
		quotient = 0;
		remainder = value;
		*inexact = remainder != 0;
	}

	if (*inexact && ((rounding_mode == 1 && sign) || (rounding_mode == 2 && !sign)))
		increment = 1;
	return quotient + (unsigned)increment;
}

static uint64_t overflow_bits(unsigned sign, unsigned rounding_mode)
{
	int to_infinity = rounding_mode == 0 ||
		(rounding_mode == 1 && sign) || (rounding_mode == 2 && !sign);
	uint64_t magnitude = to_infinity ? UINT64_C(0x7FF0000000000000) :
		UINT64_C(0x7FEFFFFFFFFFFFFF);
	return ((uint64_t)sign << 63) | magnitude;
}

int mac_guest_x87_80_load(const void *bytes10, unsigned rounding_mode, double *out)
{
	const uint8_t *bytes = bytes10;
	uint64_t significand, bits, fraction;
	uint16_t sign_exponent, exponent;
	unsigned sign;
	int unbiased;

	if (!bytes || !out || rounding_mode > 3)
		return -1;
	significand = read_le64(bytes);
	sign_exponent = read_le16(bytes + 8);
	sign = sign_exponent >> 15;
	exponent = sign_exponent & 0x7FFFu;

	if (exponent == 0x7FFFu)
	{
		if (!(significand & UINT64_C(0x8000000000000000)))
			return -1;
		fraction = (significand & UINT64_C(0x7FFFFFFFFFFFFFFF)) >> 11;
		if (!fraction && (significand & UINT64_C(0x7FFFFFFFFFFFFFFF)))
			fraction = 1;
		if (fraction && !(fraction & UINT64_C(0x0008000000000000)))
			fraction |= UINT64_C(0x0008000000000000); /* quiet signaling NaNs */
		bits = ((uint64_t)sign << 63) | UINT64_C(0x7FF0000000000000) | fraction;
		return double_from_bits(bits, out);
	}

	if (exponent == 0)
	{
		if (significand == 0)
		{
			bits = (uint64_t)sign << 63;
			return double_from_bits(bits, out);
		}
		/* Binary80 subnormals are below binary64's range; honor directed RC. */
		bits = ((uint64_t)sign << 63) |
			((rounding_mode == 1 && sign) || (rounding_mode == 2 && !sign) ? 1 : 0);
		return double_from_bits(bits, out);
	}

	/* Reject x87 unnormal encodings; accept pseudo-denormals as exponent 1. */
	if (!(significand & UINT64_C(0x8000000000000000)))
		return -1;
	unbiased = (int)(exponent ? exponent : 1) - 16383;
	fraction = significand & UINT64_C(0x7FFFFFFFFFFFFFFF);

	if (unbiased >= -1022)
	{
		uint64_t rounded;
		int inexact;
		rounded = round_right(significand, 11, sign, rounding_mode, &inexact);
		(void)inexact;
		if (rounded == (UINT64_C(1) << 53))
		{
			rounded >>= 1;
			++unbiased;
		}
		if (unbiased > 1023)
			return double_from_bits(overflow_bits(sign, rounding_mode), out);
		bits = ((uint64_t)sign << 63) |
			((uint64_t)(unbiased + 1023) << 52) |
			(rounded & UINT64_C(0x000FFFFFFFFFFFFF));
		return double_from_bits(bits, out);
	}

	/* Subnormal binary64 values are integer multiples of 2^-1074. */
	{
		int right_shift = -(unbiased + 1011);
		uint64_t units;
		int inexact;
		if (right_shift <= 0)
			return -1;
		units = round_right(significand, (unsigned)right_shift, sign, rounding_mode, &inexact);
		if (units >= (UINT64_C(1) << 52))
		{
			if (units == (UINT64_C(1) << 52))
				return double_from_bits(((uint64_t)sign << 63) | UINT64_C(0x0010000000000000), out);
			return double_from_bits(overflow_bits(sign, rounding_mode), out);
		}
		(void)inexact;
		bits = ((uint64_t)sign << 63) | units;
		return double_from_bits(bits, out);
	}
}

int mac_guest_x87_80_store(void *bytes10, double value)
{
	uint8_t *bytes = bytes10;
	uint64_t bits, significand, fraction;
	uint16_t sign_exponent;
	unsigned sign;
	int exponent;

	if (!bytes)
		return -1;
	memcpy(&bits, &value, sizeof(bits));
	sign = (unsigned)(bits >> 63);
	exponent = (int)((bits >> 52) & 0x7FFu);
	fraction = bits & UINT64_C(0x000FFFFFFFFFFFFF);

	if (exponent == 0x7FF)
	{
		if (!fraction)
			significand = UINT64_C(0x8000000000000000);
		else
		{
			/* Preserve payload and quiet bit in the wider significand. */
			significand = UINT64_C(0x8000000000000000) | (fraction << 11);
			if (!(significand & UINT64_C(0x4000000000000000)))
				significand |= UINT64_C(0x4000000000000000);
		}
		sign_exponent = (uint16_t)((sign << 15) | 0x7FFFu);
	}
	else if (!exponent && !fraction)
	{
		significand = 0;
		sign_exponent = (uint16_t)(sign << 15);
	}
	else if (exponent)
	{
		int unbiased = exponent - 1023;
		significand = (UINT64_C(1) << 63) | (fraction << 11);
		sign_exponent = (uint16_t)((sign << 15) | (unsigned)(unbiased + 16383));
	}
	else
	{
		/* Normalize a binary64 subnormal exactly into binary80. */
		unsigned top = 0;
		uint64_t mantissa = fraction;
		while (mantissa >>= 1)
			++top;
		mantissa = fraction;
		{
			int unbiased = (int)top - 1074;
			significand = mantissa << (63 - top);
			sign_exponent = (uint16_t)((sign << 15) | (unsigned)(unbiased + 16383));
		}
	}

	write_le64(bytes, significand);
	write_le16(bytes + 8, sign_exponent);
	return 0;
}
