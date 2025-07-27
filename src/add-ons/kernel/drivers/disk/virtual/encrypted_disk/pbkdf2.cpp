/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Based on OpenBSD's pkcs5_pbkdf2.c:
 * Copyright (c) 2008 Damien Bergamini <damien.bergamini@free.fr>
 */

#include <KernelExport.h>
#include <algorithm>
#include <stdlib.h>
#include <string.h>

#include "pbkdf2.hpp"
#include "sha256.hpp"

namespace Kernel::Private::crypto {

// HMAC-SHA256 implementation
static void
hmac_sha256(const uint8_t* text, size_t text_len, const uint8_t* key, size_t key_len,
	uint8_t digest[SHA256_DIGEST_LENGTH])
{
	sha256_ctx ctx;
	uint8_t k_pad[SHA256_BLOCK_LENGTH];
	uint8_t tk[SHA256_DIGEST_LENGTH];

	if (key_len > SHA256_BLOCK_LENGTH) {
		sha256_init(&ctx);
		sha256_update(&ctx, key, key_len);
		sha256_final(&ctx, tk);

		key = tk;
		key_len = SHA256_DIGEST_LENGTH;
	}

	memset(k_pad, 0, sizeof(k_pad));
	memcpy(k_pad, key, key_len);
	for (size_t i = 0; i < SHA256_BLOCK_LENGTH; i++)
		k_pad[i] ^= 0x36;

	sha256_init(&ctx);
	sha256_update(&ctx, k_pad, SHA256_BLOCK_LENGTH);
	sha256_update(&ctx, text, text_len);
	sha256_final(&ctx, digest);

	memset(k_pad, 0, sizeof(k_pad));
	memcpy(k_pad, key, key_len);
	for (size_t i = 0; i < SHA256_BLOCK_LENGTH; i++)
		k_pad[i] ^= 0x5c;

	sha256_init(&ctx);
	sha256_update(&ctx, k_pad, SHA256_BLOCK_LENGTH);
	sha256_update(&ctx, digest, SHA256_DIGEST_LENGTH);
	sha256_final(&ctx, digest);

	// Clean up sensitive data
	memset(&ctx, 0, sizeof(ctx));
	memset(k_pad, 0, sizeof(k_pad));
	memset(tk, 0, sizeof(tk));
}

/*
 * Password-Based Key Derivation Function 2 (PKCS #5 v2.0).
 * Code based on IEEE Std 802.11-2007, Annex H.4.2.
 */
status_t
pbkdf2_sha256(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
	uint8_t* key, size_t key_len, uint32_t rounds)
{
	uint8_t* asalt;
	uint8_t obuf[SHA256_DIGEST_LENGTH];
	uint8_t d1[SHA256_DIGEST_LENGTH], d2[SHA256_DIGEST_LENGTH];
	uint32_t count;
	size_t r;

	if (rounds < 1 || key_len == 0)
		return B_BAD_VALUE;
	if (salt_len == 0 || salt_len > SIZE_MAX - 4)
		return B_BAD_VALUE;

	asalt = (uint8_t*)malloc(salt_len + 4);
	if (asalt == NULL)
		return B_NO_MEMORY;

	memcpy(asalt, salt, salt_len);

	for (count = 1; key_len > 0; count++) {
		asalt[salt_len + 0] = (count >> 24) & 0xff;
		asalt[salt_len + 1] = (count >> 16) & 0xff;
		asalt[salt_len + 2] = (count >> 8) & 0xff;
		asalt[salt_len + 3] = count & 0xff;
		hmac_sha256(asalt, salt_len + 4, pass, pass_len, d1);
		memcpy(obuf, d1, sizeof(obuf));

		for (uint32_t i = 1; i < rounds; i++) {
			hmac_sha256(d1, sizeof(d1), pass, pass_len, d2);
			memcpy(d1, d2, sizeof(d1));
			for (size_t j = 0; j < sizeof(obuf); j++)
				obuf[j] ^= d1[j];
		}

		r = std::min(key_len, sizeof(obuf));
		memcpy(key, obuf, r);
		key += r;
		key_len -= r;
	}

	// Clean up sensitive data
	memset(asalt, 0, salt_len + 4);
	free(asalt);
	memset(d1, 0, sizeof(d1));
	memset(d2, 0, sizeof(d2));
	memset(obuf, 0, sizeof(obuf));

	return B_OK;
}

} // namespace Kernel::Private::crypto
