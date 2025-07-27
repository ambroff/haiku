/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Based on OpenBSD's pkcs5_pbkdf2.h
 */
#pragma once

#include <SupportDefs.h>
#include <cstddef>
#include <cstdint>

namespace Kernel::Private::crypto {

/*
 * Password-Based Key Derivation Function 2 (PKCS #5 v2.0).
 * Code based on IEEE Std 802.11-2007, Annex H.4.2.
 */
status_t pbkdf2_sha256(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
	uint8_t* key, size_t key_len, uint32_t rounds);

} // namespace Kernel::Private::crypto
