/*
 * Cryptographic API for algorithms (i.e., low-level API).
 *
 * Copyright (c) 2006 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 */
#ifndef _CRYPTO_ALGAPI_H
#define _CRYPTO_ALGAPI_H

#include <linux/crypto.h>

struct module;
struct seq_file;

struct crypto_type {
	unsigned int (*ctxsize)(struct crypto_alg *alg);
	int (*init)(struct crypto_tfm *tfm);
	void (*exit)(struct crypto_tfm *tfm);
	void (*show)(struct seq_file *m, struct crypto_alg *alg);
};

struct crypto_instance {
	struct crypto_alg alg;

	struct crypto_template *tmpl;
	struct hlist_node list;

	void *__ctx[] CRYPTO_MINALIGN_ATTR;
};

struct crypto_template {
	struct list_head list;
	struct hlist_head instances;
	struct module *module;

	struct crypto_instance *(*alloc)(void *param, unsigned int len);
	void (*free)(struct crypto_instance *inst);

	char name[CRYPTO_MAX_ALG_NAME];
};

struct crypto_spawn {
	struct list_head list;
	struct crypto_alg *alg;
	struct crypto_instance *inst;
};

struct scatter_walk {
	struct scatterlist *sg;
	unsigned int offset;
};

struct blkcipher_walk {
	union {
		struct {
			struct page *page;
			unsigned long offset;
		} phys;

		struct {
			u8 *page;
			u8 *addr;
		} virt;
	} src, dst;

	struct scatter_walk in;
	unsigned int nbytes;

	struct scatter_walk out;
	unsigned int total;

	void *page;
	u8 *buffer;
	u8 *iv;

	int flags;
};

extern const struct crypto_type crypto_blkcipher_type;
extern const struct crypto_type crypto_hash_type;

void crypto_mod_put(struct crypto_alg *alg);

int crypto_register_template(struct crypto_template *tmpl);
void crypto_unregister_template(struct crypto_template *tmpl);
struct crypto_template *crypto_lookup_template(const char *name);

int crypto_init_spawn(struct crypto_spawn *spawn, struct crypto_alg *alg,
		      struct crypto_instance *inst);
void crypto_drop_spawn(struct crypto_spawn *spawn);
struct crypto_tfm *crypto_spawn_tfm(struct crypto_spawn *spawn);

struct crypto_alg *crypto_get_attr_alg(void *param, unsigned int len,
				       u32 type, u32 mask);
struct crypto_instance *crypto_alloc_instance(const char *name,
					      struct crypto_alg *alg);

int blkcipher_walk_done(struct blkcipher_desc *desc,
			struct blkcipher_walk *walk, int err);
int blkcipher_walk_virt(struct blkcipher_desc *desc,
			struct blkcipher_walk *walk);
int blkcipher_walk_phys(struct blkcipher_desc *desc,
			struct blkcipher_walk *walk);

static inline void *crypto_tfm_ctx_aligned(struct crypto_tfm *tfm)
{
	unsigned long addr = (unsigned long)crypto_tfm_ctx(tfm);
	unsigned long align = crypto_tfm_alg_alignmask(tfm);

	if (align <= crypto_tfm_ctx_alignment())
		align = 1;
	return (void *)ALIGN(addr, align);
}

static inline void *crypto_instance_ctx(struct crypto_instance *inst)
{
	return inst->__ctx;
}

static inline void *crypto_blkcipher_ctx(struct crypto_blkcipher *tfm)
{
	return crypto_tfm_ctx(&tfm->base);
}

static inline void *crypto_blkcipher_ctx_aligned(struct crypto_blkcipher *tfm)
{
	return crypto_tfm_ctx_aligned(&tfm->base);
}

static inline struct cipher_alg *crypto_cipher_alg(struct crypto_cipher *tfm)
{
	return &crypto_cipher_tfm(tfm)->__crt_alg->cra_cipher;
}

static inline void *crypto_hash_ctx_aligned(struct crypto_hash *tfm)
{
	return crypto_tfm_ctx_aligned(&tfm->base);
}

static inline void blkcipher_walk_init(struct blkcipher_walk *walk,
				       struct scatterlist *dst,
				       struct scatterlist *src,
				       unsigned int nbytes)
{
	walk->in.sg = src;
	walk->out.sg = dst;
	walk->total = nbytes;
}

#endif	/* _CRYPTO_ALGAPI_H */

