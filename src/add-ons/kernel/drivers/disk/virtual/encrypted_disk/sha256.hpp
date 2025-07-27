/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Minimal SHA256 wrapper for PBKDF2
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Private::crypto {

constexpr size_t SHA256_BLOCK_LENGTH = 64;
constexpr size_t SHA256_DIGEST_LENGTH = 32;

struct sha256_ctx {
	uint32_t state[8];
	uint64_t count;
	uint8_t buffer[SHA256_BLOCK_LENGTH];
};

void sha256_init(sha256_ctx* ctx);
void sha256_update(sha256_ctx* ctx, const uint8_t* data, size_t len);
void sha256_final(sha256_ctx* ctx, uint8_t* digest);

} // namespace Kernel::Private::crypto
