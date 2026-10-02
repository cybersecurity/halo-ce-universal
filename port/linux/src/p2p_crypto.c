/*
P2P_CRYPTO.C

What internet play needs to keep an invite's signalling and its tunnel
private (p2p.c): SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), from
which an invite's topics and keys are derived; ChaCha20-Poly1305 (RFC 8439)
with additional data, which seals the tunnel's packets (a counter for their
nonce) and, with a random nonce sent ahead of the ciphertext, signalling's
messages; and X25519 (RFC 7748), with which two machines agree on their
tunnel's keys without sending them. The public brokers carry sealed
messages, so only holders of the invite read them; the tunnel's packets are
sealed with keys only its two machines have.
*/

#ifdef HALO_RELAY
#include "relay_platform.h"
#else
#include "platform.h"
#endif
#include "posix.h"
#include "p2p_internal.h"

#include <string.h>

/* ---------- SHA-256 */

struct sha256
{
	unsigned int state[8];
	unsigned char block[64];
	unsigned long long length;
	int used;
};

static const unsigned int sha256_constants[64] =
{
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTATE_RIGHT(value, count) (((value) >> (count)) | ((value) << (32 - (count))))

static void sha256_block(struct sha256 *context, const unsigned char *block)
{
	unsigned int words[64];
	unsigned int a, b, c, d, e, f, g, h;
	int index;

	for (index = 0; index < 16; index++)
	{
		words[index] = (unsigned int)block[index * 4] << 24 | (unsigned int)block[index * 4 + 1] << 16 |
			(unsigned int)block[index * 4 + 2] << 8 | (unsigned int)block[index * 4 + 3];
	}
	for (index = 16; index < 64; index++)
	{
		unsigned int s0 = ROTATE_RIGHT(words[index - 15], 7) ^ ROTATE_RIGHT(words[index - 15], 18) ^
			(words[index - 15] >> 3);
		unsigned int s1 = ROTATE_RIGHT(words[index - 2], 17) ^ ROTATE_RIGHT(words[index - 2], 19) ^
			(words[index - 2] >> 10);

		words[index] = words[index - 16] + s0 + words[index - 7] + s1;
	}
	a = context->state[0]; b = context->state[1]; c = context->state[2]; d = context->state[3];
	e = context->state[4]; f = context->state[5]; g = context->state[6]; h = context->state[7];
	for (index = 0; index < 64; index++)
	{
		unsigned int s1 = ROTATE_RIGHT(e, 6) ^ ROTATE_RIGHT(e, 11) ^ ROTATE_RIGHT(e, 25);
		unsigned int choice = (e & f) ^ (~e & g);
		unsigned int first = h + s1 + choice + sha256_constants[index] + words[index];
		unsigned int s0 = ROTATE_RIGHT(a, 2) ^ ROTATE_RIGHT(a, 13) ^ ROTATE_RIGHT(a, 22);
		unsigned int majority = (a & b) ^ (a & c) ^ (b & c);
		unsigned int second = s0 + majority;

		h = g; g = f; f = e; e = d + first;
		d = c; c = b; b = a; a = first + second;
	}
	context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
	context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

static void sha256_begin(struct sha256 *context)
{
	static const unsigned int initial[8] =
	{
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(context->state, initial, sizeof(initial));
	context->length = 0;
	context->used = 0;
}

static void sha256_add(struct sha256 *context, const void *data, int size)
{
	const unsigned char *bytes = data;

	context->length += (unsigned long long)size;
	while (size > 0)
	{
		int count = 64 - context->used < size ? 64 - context->used : size;

		memcpy(context->block + context->used, bytes, (size_t)count);
		context->used += count;
		bytes += count;
		size -= count;
		if (context->used == 64)
		{
			sha256_block(context, context->block);
			context->used = 0;
		}
	}
}

static void sha256_end(struct sha256 *context, unsigned char *digest)
{
	unsigned long long bits = context->length * 8;
	unsigned char length[8];
	int index;

	for (index = 0; index < 8; index++)
		length[index] = (unsigned char)(bits >> (56 - index * 8));
	sha256_add(context, "\x80", 1);
	while (context->used != 56)
		sha256_add(context, "", 1);
	sha256_add(context, length, 8);
	for (index = 0; index < 8; index++)
	{
		digest[index * 4] = (unsigned char)(context->state[index] >> 24);
		digest[index * 4 + 1] = (unsigned char)(context->state[index] >> 16);
		digest[index * 4 + 2] = (unsigned char)(context->state[index] >> 8);
		digest[index * 4 + 3] = (unsigned char)context->state[index];
	}
}

void p2p_sha256(const void *data, int size, unsigned char *digest)
{
	struct sha256 context;

	sha256_begin(&context);
	sha256_add(&context, data, size);
	sha256_end(&context, digest);
}

void p2p_hmac_sha256(const unsigned char *key, int key_size, const void *data, int size, unsigned char *digest)
{
	unsigned char block[64], inner[P2P_SHA256_SIZE];
	struct sha256 context;
	int index;

	memset(block, 0, sizeof(block));
	if (key_size > 64)
		p2p_sha256(key, key_size, block);
	else
		memcpy(block, key, (size_t)key_size);
	for (index = 0; index < 64; index++)
		block[index] ^= 0x36;
	sha256_begin(&context);
	sha256_add(&context, block, 64);
	sha256_add(&context, data, size);
	sha256_end(&context, inner);
	for (index = 0; index < 64; index++)
		block[index] ^= 0x36 ^ 0x5c;
	sha256_begin(&context);
	sha256_add(&context, block, 64);
	sha256_add(&context, inner, sizeof(inner));
	sha256_end(&context, digest);
}

/* ---------- ChaCha20 and Poly1305 (RFC 8439) */

static unsigned int load32(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8 | (unsigned int)bytes[2] << 16 |
		(unsigned int)bytes[3] << 24;
}

static void store32(unsigned char *bytes, unsigned int value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

#define ROTATE_LEFT(value, count) (((value) << (count)) | ((value) >> (32 - (count))))
#define QUARTER_ROUND(a, b, c, d) \
	a += b; d ^= a; d = ROTATE_LEFT(d, 16); \
	c += d; b ^= c; b = ROTATE_LEFT(b, 12); \
	a += b; d ^= a; d = ROTATE_LEFT(d, 8); \
	c += d; b ^= c; b = ROTATE_LEFT(b, 7)

static void chacha20_block(const unsigned char *key, unsigned int counter, const unsigned char *nonce,
	unsigned char *output)
{
	unsigned int state[16], x[16];
	int index;

	state[0] = 0x61707865;
	state[1] = 0x3320646e;
	state[2] = 0x79622d32;
	state[3] = 0x6b206574;
	for (index = 0; index < 8; index++)
		state[4 + index] = load32(key + index * 4);
	state[12] = counter;
	state[13] = load32(nonce);
	state[14] = load32(nonce + 4);
	state[15] = load32(nonce + 8);
	memcpy(x, state, sizeof(x));
	for (index = 0; index < 10; index++)
	{
		QUARTER_ROUND(x[0], x[4], x[8], x[12]);
		QUARTER_ROUND(x[1], x[5], x[9], x[13]);
		QUARTER_ROUND(x[2], x[6], x[10], x[14]);
		QUARTER_ROUND(x[3], x[7], x[11], x[15]);
		QUARTER_ROUND(x[0], x[5], x[10], x[15]);
		QUARTER_ROUND(x[1], x[6], x[11], x[12]);
		QUARTER_ROUND(x[2], x[7], x[8], x[13]);
		QUARTER_ROUND(x[3], x[4], x[9], x[14]);
	}
	for (index = 0; index < 16; index++)
		store32(output + index * 4, x[index] + state[index]);
}

/* output = input ^ the keystream from block counter on */
static void chacha20_xor(const unsigned char *key, unsigned int counter, const unsigned char *nonce,
	const unsigned char *input, int size, unsigned char *output)
{
	unsigned char block[64];
	int offset;

	for (offset = 0; offset < size; offset += 64, counter++)
	{
		int index;

		chacha20_block(key, counter, nonce, block);
		for (index = 0; index < 64 && offset + index < size; index++)
			output[offset + index] = input[offset + index] ^ block[index];
	}
}

/* Poly1305 in 26-bit limbs (after Andrew Moon's poly1305-donna) */
struct poly1305
{
	unsigned int r[5];
	unsigned int h[5];
	unsigned int pad[4];
};

static void poly1305_begin(struct poly1305 *context, const unsigned char *key)
{
	context->r[0] = load32(key) & 0x3ffffff;
	context->r[1] = (load32(key + 3) >> 2) & 0x3ffff03;
	context->r[2] = (load32(key + 6) >> 4) & 0x3ffc0ff;
	context->r[3] = (load32(key + 9) >> 6) & 0x3f03fff;
	context->r[4] = (load32(key + 12) >> 8) & 0x00fffff;
	memset(context->h, 0, sizeof(context->h));
	context->pad[0] = load32(key + 16);
	context->pad[1] = load32(key + 20);
	context->pad[2] = load32(key + 24);
	context->pad[3] = load32(key + 28);
}

/* whole 16-byte blocks; a shorter message is padded with zeros, as RFC
8439's AEAD pads each part */
static void poly1305_add(struct poly1305 *context, const unsigned char *data, int size)
{
	const unsigned int r0 = context->r[0], r1 = context->r[1], r2 = context->r[2], r3 = context->r[3],
		r4 = context->r[4];
	const unsigned int s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
	unsigned int h0 = context->h[0], h1 = context->h[1], h2 = context->h[2], h3 = context->h[3],
		h4 = context->h[4];

	while (size > 0)
	{
		unsigned char block[16];
		unsigned long long d0, d1, d2, d3, d4;
		unsigned int carry;

		if (size < 16)
		{
			memset(block, 0, sizeof(block));
			memcpy(block, data, (size_t)size);
			data = block;
			size = 16;
		}
		h0 += load32(data) & 0x3ffffff;
		h1 += (load32(data + 3) >> 2) & 0x3ffffff;
		h2 += (load32(data + 6) >> 4) & 0x3ffffff;
		h3 += (load32(data + 9) >> 6) & 0x3ffffff;
		h4 += (load32(data + 12) >> 8) | (1u << 24);
		d0 = (unsigned long long)h0 * r0 + (unsigned long long)h1 * s4 + (unsigned long long)h2 * s3 +
			(unsigned long long)h3 * s2 + (unsigned long long)h4 * s1;
		d1 = (unsigned long long)h0 * r1 + (unsigned long long)h1 * r0 + (unsigned long long)h2 * s4 +
			(unsigned long long)h3 * s3 + (unsigned long long)h4 * s2;
		d2 = (unsigned long long)h0 * r2 + (unsigned long long)h1 * r1 + (unsigned long long)h2 * r0 +
			(unsigned long long)h3 * s4 + (unsigned long long)h4 * s3;
		d3 = (unsigned long long)h0 * r3 + (unsigned long long)h1 * r2 + (unsigned long long)h2 * r1 +
			(unsigned long long)h3 * r0 + (unsigned long long)h4 * s4;
		d4 = (unsigned long long)h0 * r4 + (unsigned long long)h1 * r3 + (unsigned long long)h2 * r2 +
			(unsigned long long)h3 * r1 + (unsigned long long)h4 * r0;
		carry = (unsigned int)(d0 >> 26); h0 = (unsigned int)d0 & 0x3ffffff;
		d1 += carry; carry = (unsigned int)(d1 >> 26); h1 = (unsigned int)d1 & 0x3ffffff;
		d2 += carry; carry = (unsigned int)(d2 >> 26); h2 = (unsigned int)d2 & 0x3ffffff;
		d3 += carry; carry = (unsigned int)(d3 >> 26); h3 = (unsigned int)d3 & 0x3ffffff;
		d4 += carry; carry = (unsigned int)(d4 >> 26); h4 = (unsigned int)d4 & 0x3ffffff;
		h0 += carry * 5; carry = h0 >> 26; h0 &= 0x3ffffff;
		h1 += carry;
		data += 16;
		size -= 16;
	}
	context->h[0] = h0; context->h[1] = h1; context->h[2] = h2; context->h[3] = h3; context->h[4] = h4;
}

static void poly1305_end(struct poly1305 *context, unsigned char *tag)
{
	unsigned int h0 = context->h[0], h1 = context->h[1], h2 = context->h[2], h3 = context->h[3],
		h4 = context->h[4];
	unsigned int g0, g1, g2, g3, g4, carry, mask;
	unsigned long long sum;

	carry = h1 >> 26; h1 &= 0x3ffffff;
	h2 += carry; carry = h2 >> 26; h2 &= 0x3ffffff;
	h3 += carry; carry = h3 >> 26; h3 &= 0x3ffffff;
	h4 += carry; carry = h4 >> 26; h4 &= 0x3ffffff;
	h0 += carry * 5; carry = h0 >> 26; h0 &= 0x3ffffff;
	h1 += carry;
	/* h - p, chosen in constant time if h >= p */
	g0 = h0 + 5; carry = g0 >> 26; g0 &= 0x3ffffff;
	g1 = h1 + carry; carry = g1 >> 26; g1 &= 0x3ffffff;
	g2 = h2 + carry; carry = g2 >> 26; g2 &= 0x3ffffff;
	g3 = h3 + carry; carry = g3 >> 26; g3 &= 0x3ffffff;
	g4 = h4 + carry - (1u << 26);
	mask = (g4 >> 31) - 1;
	h0 = (h0 & ~mask) | (g0 & mask);
	h1 = (h1 & ~mask) | (g1 & mask);
	h2 = (h2 & ~mask) | (g2 & mask);
	h3 = (h3 & ~mask) | (g3 & mask);
	h4 = (h4 & ~mask) | (g4 & mask);
	/* h + pad, modulo 2^128 */
	h0 = h0 | h1 << 26;
	h1 = h1 >> 6 | h2 << 20;
	h2 = h2 >> 12 | h3 << 14;
	h3 = h3 >> 18 | h4 << 8;
	sum = (unsigned long long)h0 + context->pad[0]; store32(tag, (unsigned int)sum);
	sum = (unsigned long long)h1 + context->pad[1] + (sum >> 32); store32(tag + 4, (unsigned int)sum);
	sum = (unsigned long long)h2 + context->pad[2] + (sum >> 32); store32(tag + 8, (unsigned int)sum);
	sum = (unsigned long long)h3 + context->pad[3] + (sum >> 32); store32(tag + 12, (unsigned int)sum);
}

/* ---------- ChaCha20-Poly1305 (RFC 8439) */

/* the AEAD's tag over the additional data and the ciphertext */
static void aead_tag(const unsigned char *key, const unsigned char *nonce, const unsigned char *additional,
	int additional_size, const unsigned char *ciphertext, int size, unsigned char *tag)
{
	unsigned char block[64];
	unsigned char lengths[16];
	struct poly1305 context;

	chacha20_block(key, 0, nonce, block);
	poly1305_begin(&context, block);
	poly1305_add(&context, additional, additional_size);
	poly1305_add(&context, ciphertext, size);
	memset(lengths, 0, sizeof(lengths));
	store32(lengths, (unsigned int)additional_size);
	store32(lengths + 8, (unsigned int)size);
	poly1305_add(&context, lengths, sizeof(lengths));
	poly1305_end(&context, tag);
}

int p2p_aead_seal(const unsigned char *key, const unsigned char *nonce, const void *additional,
	int additional_size, const void *plaintext, int size, unsigned char *sealed)
{
	chacha20_xor(key, 1, nonce, plaintext, size, sealed);
	aead_tag(key, nonce, additional, additional_size, sealed, size, sealed + size);
	return size + P2P_TAG_SIZE;
}

int p2p_aead_open(const unsigned char *key, const unsigned char *nonce, const void *additional,
	int additional_size, const unsigned char *sealed, int size, unsigned char *plaintext)
{
	unsigned char tag[P2P_TAG_SIZE];
	int text_size = size - P2P_TAG_SIZE;

	if (text_size < 0)
		return -1;
	aead_tag(key, nonce, additional, additional_size, sealed, text_size, tag);
	if (!p2p_equal(tag, sealed + text_size, P2P_TAG_SIZE))
		return -1;
	chacha20_xor(key, 1, nonce, sealed, text_size, plaintext);
	return text_size;
}

int p2p_seal(const unsigned char *key, const void *plaintext, int size, unsigned char *sealed)
{
	posix_random_bytes(sealed, P2P_NONCE_SIZE);
	return P2P_NONCE_SIZE + p2p_aead_seal(key, sealed, NULL, 0, plaintext, size, sealed + P2P_NONCE_SIZE);
}

int p2p_open(const unsigned char *key, const unsigned char *sealed, int size, unsigned char *plaintext)
{
	if (size < P2P_NONCE_SIZE)
		return -1;
	return p2p_aead_open(key, sealed, NULL, 0, sealed + P2P_NONCE_SIZE, size - P2P_NONCE_SIZE, plaintext);
}

int p2p_equal(const void *first, const void *second, int size)
{
	const unsigned char *a = first, *b = second;
	unsigned char difference = 0;
	int index;

	/* in constant time */
	for (index = 0; index < size; index++)
		difference |= (unsigned char)(a[index] ^ b[index]);
	return difference == 0;
}

/* ---------- X25519 (RFC 7748), after TweetNaCl's crypto_scalarmult
(Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers; public domain):
field elements in sixteen 16-bit limbs, every step in constant time */

typedef long long field[16];

static void field_carry(field value)
{
	long long carry;
	int index;

	for (index = 0; index < 16; index++)
	{
		value[index] += 1LL << 16;
		carry = value[index] >> 16;
		value[(index + 1) * (index < 15)] += carry - 1 + 37 * (carry - 1) * (index == 15);
		value[index] -= carry * 65536;
	}
}

/* swaps p and q if bit, without branching on it */
static void field_swap(field p, field q, long long bit)
{
	long long mask = ~(bit - 1);
	int index;

	for (index = 0; index < 16; index++)
	{
		long long t = mask & (p[index] ^ q[index]);

		p[index] ^= t;
		q[index] ^= t;
	}
}

static void field_pack(unsigned char *bytes, const field value)
{
	field m, t;
	int index, pass;

	memcpy(t, value, sizeof(t));
	field_carry(t);
	field_carry(t);
	field_carry(t);
	for (pass = 0; pass < 2; pass++)
	{
		long long borrow;

		m[0] = t[0] - 0xffed;
		for (index = 1; index < 15; index++)
		{
			m[index] = t[index] - 0xffff - ((m[index - 1] >> 16) & 1);
			m[index - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		borrow = (m[15] >> 16) & 1;
		m[14] &= 0xffff;
		field_swap(t, m, 1 - borrow);
	}
	for (index = 0; index < 16; index++)
	{
		bytes[2 * index] = (unsigned char)(t[index] & 0xff);
		bytes[2 * index + 1] = (unsigned char)(t[index] >> 8);
	}
}

static void field_unpack(field value, const unsigned char *bytes)
{
	int index;

	for (index = 0; index < 16; index++)
		value[index] = bytes[2 * index] + ((long long)bytes[2 * index + 1] << 8);
	value[15] &= 0x7fff;
}

static void field_add(field out, const field a, const field b)
{
	int index;

	for (index = 0; index < 16; index++)
		out[index] = a[index] + b[index];
}

static void field_subtract(field out, const field a, const field b)
{
	int index;

	for (index = 0; index < 16; index++)
		out[index] = a[index] - b[index];
}

static void field_multiply(field out, const field a, const field b)
{
	long long t[31];
	int i, j;

	memset(t, 0, sizeof(t));
	for (i = 0; i < 16; i++)
	{
		for (j = 0; j < 16; j++)
			t[i + j] += a[i] * b[j];
	}
	for (i = 0; i < 15; i++)
		t[i] += 38 * t[i + 16];
	memcpy(out, t, sizeof(field));
	field_carry(out);
	field_carry(out);
}

static void field_invert(field out, const field value)
{
	field c;
	int bit;

	memcpy(c, value, sizeof(c));
	for (bit = 253; bit >= 0; bit--)
	{
		field_multiply(c, c, c);
		if (bit != 2 && bit != 4)
			field_multiply(c, c, value);
	}
	memcpy(out, c, sizeof(c));
}

void p2p_x25519(unsigned char *result, const unsigned char *scalar, const unsigned char *point)
{
	static const unsigned char base_point[P2P_KEY_SIZE] = { 9 };
	static const field a24 = { 0xDB41, 1 };
	unsigned char z[P2P_KEY_SIZE];
	field x, a, b, c, d, e, f;
	int index;

	memcpy(z, scalar, sizeof(z));
	z[31] = (unsigned char)((z[31] & 127) | 64);
	z[0] &= 248;
	field_unpack(x, point ? point : base_point);
	memcpy(b, x, sizeof(b));
	memset(a, 0, sizeof(a));
	memset(c, 0, sizeof(c));
	memset(d, 0, sizeof(d));
	a[0] = d[0] = 1;
	for (index = 254; index >= 0; index--)
	{
		long long bit = (z[index >> 3] >> (index & 7)) & 1;

		field_swap(a, b, bit);
		field_swap(c, d, bit);
		field_add(e, a, c);
		field_subtract(a, a, c);
		field_add(c, b, d);
		field_subtract(b, b, d);
		field_multiply(d, e, e);
		field_multiply(f, a, a);
		field_multiply(a, c, a);
		field_multiply(c, b, e);
		field_add(e, a, c);
		field_subtract(a, a, c);
		field_multiply(b, a, a);
		field_subtract(c, d, f);
		field_multiply(a, c, a24);
		field_add(a, a, d);
		field_multiply(c, c, a);
		field_multiply(a, d, f);
		field_multiply(d, b, x);
		field_multiply(b, e, e);
		field_swap(a, b, bit);
		field_swap(c, d, bit);
	}
	field_invert(c, c);
	field_multiply(a, a, c);
	field_pack(result, a);
}
