/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Minimal SHA256 implementation for PBKDF2
 * Based on public domain implementation by Aaron D. Gifford
 */

#include "sha256.hpp"
#include <string.h>

namespace Kernel::Private::crypto {

// SHA256 constants
static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
	0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
	0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3,
	0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354,
	0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3,
	0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
	0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// Initial hash values
static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
	0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

// Rotate right
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// SHA256 operations
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIGMA0(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define SIGMA1(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define sigma0(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define sigma1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

// Convert big-endian bytes to uint32
static inline uint32_t
be32dec(const void* pp)
{
	const uint8_t* p = (const uint8_t*)pp;
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Convert uint32 to big-endian bytes
static inline void
be32enc(void* pp, uint32_t x)
{
	uint8_t* p = (uint8_t*)pp;
	p[0] = (x >> 24) & 0xff;
	p[1] = (x >> 16) & 0xff;
	p[2] = (x >> 8) & 0xff;
	p[3] = x & 0xff;
}

// SHA256 compression function
static void
sha256_transform(uint32_t* state, const uint8_t block[SHA256_BLOCK_LENGTH])
{
	uint32_t W[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t T1, T2;
	int i;

	// Initialize first 16 words of W
	for (i = 0; i < 16; i++)
		W[i] = be32dec(&block[i * 4]);

	// Extend the first 16 words into the remaining 48 words
	for (i = 16; i < 64; i++)
		W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];

	// Initialize working variables
	a = state[0];
	b = state[1];
	c = state[2];
	d = state[3];
	e = state[4];
	f = state[5];
	g = state[6];
	h = state[7];

	// Main loop
	for (i = 0; i < 64; i++) {
		T1 = h + SIGMA1(e) + CH(e, f, g) + K[i] + W[i];
		T2 = SIGMA0(a) + MAJ(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + T1;
		d = c;
		c = b;
		b = a;
		a = T1 + T2;
	}

	// Add compressed chunk to current hash value
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}


void
sha256_init(sha256_ctx* ctx)
{
	// Set initial hash values
	for (int i = 0; i < 8; i++)
		ctx->state[i] = H0[i];
	ctx->count = 0;
}


void
sha256_update(sha256_ctx* ctx, const uint8_t* data, size_t len)
{
	size_t i;

	// Current position in buffer
	i = (size_t)(ctx->count & (SHA256_BLOCK_LENGTH - 1));
	ctx->count += len;

	// Process any partial block
	if (i > 0) {
		size_t copy = SHA256_BLOCK_LENGTH - i;
		if (copy > len)
			copy = len;
		memcpy(&ctx->buffer[i], data, copy);
		data += copy;
		len -= copy;
		i += copy;

		if (i == SHA256_BLOCK_LENGTH) {
			sha256_transform(ctx->state, ctx->buffer);
			i = 0;
		}
	}

	// Process full blocks
	while (len >= SHA256_BLOCK_LENGTH) {
		sha256_transform(ctx->state, data);
		data += SHA256_BLOCK_LENGTH;
		len -= SHA256_BLOCK_LENGTH;
	}

	// Save any remaining bytes
	if (len > 0)
		memcpy(ctx->buffer, data, len);
}


void
sha256_final(sha256_ctx* ctx, uint8_t* digest)
{
	size_t i;
	uint64_t bitcount;

	// Current position in buffer
	i = (size_t)(ctx->count & (SHA256_BLOCK_LENGTH - 1));

	// Pad the message
	ctx->buffer[i++] = 0x80;

	// If not enough room for length, process block and continue
	if (i > SHA256_BLOCK_LENGTH - 8) {
		memset(&ctx->buffer[i], 0, SHA256_BLOCK_LENGTH - i);
		sha256_transform(ctx->state, ctx->buffer);
		i = 0;
	}

	// Pad with zeros and add bit count
	memset(&ctx->buffer[i], 0, SHA256_BLOCK_LENGTH - 8 - i);
	bitcount = ctx->count * 8;
	be32enc(&ctx->buffer[SHA256_BLOCK_LENGTH - 8], (uint32_t)(bitcount >> 32));
	be32enc(&ctx->buffer[SHA256_BLOCK_LENGTH - 4], (uint32_t)bitcount);

	// Final transform
	sha256_transform(ctx->state, ctx->buffer);

	// Convert state to digest
	for (i = 0; i < 8; i++)
		be32enc(&digest[i * 4], ctx->state[i]);

	// Clear sensitive information
	memset(ctx, 0, sizeof(*ctx));
}

} // namespace Kernel::Private::crypto
