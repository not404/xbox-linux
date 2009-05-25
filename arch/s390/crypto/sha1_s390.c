/*
 * Cryptographic API.
 *
 * s390 implementation of the SHA1 Secure Hash Algorithm.
 *
 * Derived from cryptoapi implementation, adapted for in-place
 * scatterlist interface.  Originally based on the public domain
 * implementation written by Steve Reid.
 *
 * s390 Version:
 *   Copyright IBM Corp. 2003,2007
 *   Author(s): Thomas Spatzier
 *		Jan Glauber (jan.glauber@de.ibm.com)
 *
 * Derived from "crypto/sha1.c"
 *   Copyright (c) Alan Smithee.
 *   Copyright (c) Andrew McDonald <andrew@mcdonald.org.uk>
 *   Copyright (c) Jean-Francois Dive <jef@linuxbe.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/crypto.h>
#include <asm/scatterlist.h>
#include <asm/byteorder.h>
#include "crypt_s390.h"

#define SHA1_DIGEST_SIZE	20
#define SHA1_BLOCK_SIZE		64

struct crypt_s390_sha1_ctx {
	u64 count;
	u32 state[5];
	u32 buf_len;
	u8 buffer[2 * SHA1_BLOCK_SIZE];
};

static void sha1_init(struct crypto_tfm *tfm)
{
	struct crypt_s390_sha1_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->state[0] = 0x67452301;
	ctx->state[1] =	0xEFCDAB89;
	ctx->state[2] =	0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] =	0xC3D2E1F0;

	ctx->count = 0;
	ctx->buf_len = 0;
}

static void sha1_update(struct crypto_tfm *tfm, const u8 *data,
			unsigned int len)
{
	struct crypt_s390_sha1_ctx *sctx;
	long imd_len;

	sctx = crypto_tfm_ctx(tfm);
	sctx->count += len * 8; /* message bit length */

	/* anything in buffer yet? -> must be completed */
	if (sctx->buf_len && (sctx->buf_len + len) >= SHA1_BLOCK_SIZE) {
		/* complete full block and hash */
		memcpy(sctx->buffer + sctx->buf_len, data,
		       SHA1_BLOCK_SIZE - sctx->buf_len);
		crypt_s390_kimd(KIMD_SHA_1, sctx->state, sctx->buffer,
				SHA1_BLOCK_SIZE);
		data += SHA1_BLOCK_SIZE - sctx->buf_len;
		len -= SHA1_BLOCK_SIZE - sctx->buf_len;
		sctx->buf_len = 0;
	}

	/* rest of data contains full blocks? */
	imd_len = len & ~0x3ful;
	if (imd_len) {
		crypt_s390_kimd(KIMD_SHA_1, sctx->state, data, imd_len);
		data += imd_len;
		len -= imd_len;
	}
	/* anything left? store in buffer */
	if (len) {
		memcpy(sctx->buffer + sctx->buf_len , data, len);
		sctx->buf_len += len;
	}
}


static void pad_message(struct crypt_s390_sha1_ctx* sctx)
{
	int index;

	index = sctx->buf_len;
	sctx->buf_len = (sctx->buf_len < 56) ?
			 SHA1_BLOCK_SIZE:2 * SHA1_BLOCK_SIZE;
	/* start pad with 1 */
	sctx->buffer[index] = 0x80;
	/* pad with zeros */
	index++;
	memset(sctx->buffer + index, 0x00, sctx->buf_len - index);
	/* append length */
	memcpy(sctx->buffer + sctx->buf_len - 8, &sctx->count,
	       sizeof sctx->count);
}

/* Add padding and return the message digest. */
static void sha1_final(struct crypto_tfm *tfm, u8 *out)
{
	struct crypt_s390_sha1_ctx *sctx = crypto_tfm_ctx(tfm);

	/* must perform manual padding */
	pad_message(sctx);
	crypt_s390_kimd(KIMD_SHA_1, sctx->state, sctx->buffer, sctx->buf_len);
	/* copy digest to out */
	memcpy(out, sctx->state, SHA1_DIGEST_SIZE);
	/* wipe context */
	memset(sctx, 0, sizeof *sctx);
}

static struct crypto_alg alg = {
	.cra_name	=	"sha1",
	.cra_driver_name=	"sha1-s390",
	.cra_priority	=	CRYPT_S390_PRIORITY,
	.cra_flags	=	CRYPTO_ALG_TYPE_DIGEST,
	.cra_blocksize	=	SHA1_BLOCK_SIZE,
	.cra_ctxsize	=	sizeof(struct crypt_s390_sha1_ctx),
	.cra_module	=	THIS_MODULE,
	.cra_list	=	LIST_HEAD_INIT(alg.cra_list),
	.cra_u		=	{ .digest = {
	.dia_digestsize	=	SHA1_DIGEST_SIZE,
	.dia_init	=	sha1_init,
	.dia_update	=	sha1_update,
	.dia_final	=	sha1_final } }
};

static int __init init(void)
{
	if (!crypt_s390_func_available(KIMD_SHA_1))
		return -EOPNOTSUPP;

	return crypto_register_alg(&alg);
}

static void __exit fini(void)
{
	crypto_unregister_alg(&alg);
}

module_init(init);
module_exit(fini);

MODULE_ALIAS("sha1");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SHA1 Secure Hash Algorithm");
