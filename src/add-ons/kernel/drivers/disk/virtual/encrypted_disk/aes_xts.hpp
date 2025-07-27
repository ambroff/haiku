/*	$OpenBSD: aes_xts.h,v 1.1 2012/10/09 12:36:50 jsing Exp $	*/
/*
 * Copyright (C) 2008, Damien Miller
 *
 * Permission to use, copy, and modify this software with or without fee
 * is hereby granted, provided that this entire notice is included in
 * all copies of any software which is or includes a copy or
 * modification of this software.
 * You may use this code under the GNU public license if you so wish. Please
 * contribute changes back to the authors under this freer than GPL license
 * so that we may further the use of strong encryption without limitations to
 * all.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTY. IN PARTICULAR, NONE OF THE AUTHORS MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE
 * MERCHANTABILITY OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR
 * PURPOSE.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "rijndael.hpp"

namespace Kernel::Private::crypto {

constexpr std::size_t AES_XTS_BLOCKSIZE = 16;
constexpr std::size_t AES_XTS_IVSIZE = 8;
constexpr std::size_t AES_XTS_ALPHA = 0x87; /* GF(2^128) generator polynomial */

struct aes_xts_ctx {
	rijndael_ctx key1;
	rijndael_ctx key2;
	uint8_t tweak[AES_XTS_BLOCKSIZE];
};

int aes_xts_setkey(struct aes_xts_ctx*, uint8_t*, int);
void aes_xts_crypt(struct aes_xts_ctx*, uint8_t*, unsigned int);
void aes_xts_encrypt(struct aes_xts_ctx*, uint8_t*);
void aes_xts_decrypt(struct aes_xts_ctx*, uint8_t*);
void aes_xts_zerokey(struct aes_xts_ctx*);
void aes_xts_reinit(struct aes_xts_ctx*, uint8_t*);

} // namespace Kernel::Private::crypto
