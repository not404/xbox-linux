/*
 *  linux/fs/fatx/cache.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  FATX port 2005 by Edgar Hucek
 *
 *  Mar 1999. AV. Changed cache, so that it uses the starting cluster instead
 *	of inode number.
 *  May 1999. AV. Fixed the bogosity with FAT32 (read "FAT28"). Fscking lusers.
 */

#include <linux/fs.h>
#include <linux/fatx_fs.h>
#include <linux/buffer_head.h>

/* this must be > 0. */
#define FAT_MAX_CACHE	8

extern unsigned int fatx_debug;

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

struct fatx_cache {
	struct list_head cache_list;
	int nr_contig;	/* number of contiguous clusters */
	int fcluster;	/* cluster number in the file. */
	int dcluster;	/* cluster number on disk. */
};

struct fatx_cache_id {
	unsigned int id;
	int nr_contig;
	int fcluster;
	int dcluster;
};

static inline int fatx_max_cache(struct inode *inode)
{
	return FAT_MAX_CACHE;
}

static kmem_cache_t *fatx_cache_cachep;

static void init_once(void *foo, kmem_cache_t *cachep, unsigned long flags)
{
	struct fatx_cache *cache = (struct fatx_cache *)foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR)
		INIT_LIST_HEAD(&cache->cache_list);
}

int __init fatx_cache_init(void)
{
	fatx_cache_cachep = kmem_cache_create("fatx_cache",
				sizeof(struct fatx_cache),
				0, SLAB_RECLAIM_ACCOUNT,
				init_once, NULL);
	if (fatx_cache_cachep == NULL)
		return -ENOMEM;
	return 0;
}

void __exit fatx_cache_destroy(void)
{
	if (kmem_cache_destroy(fatx_cache_cachep))
		printk(KERN_INFO "fatx_cache: not all structures were freed\n");
}

static inline struct fatx_cache *fatx_cache_alloc(struct inode *inode)
{
	return kmem_cache_alloc(fatx_cache_cachep, SLAB_KERNEL);
}

static inline void fatx_cache_free(struct fatx_cache *cache)
{
	BUG_ON(!list_empty(&cache->cache_list));
	kmem_cache_free(fatx_cache_cachep, cache);
}

static inline void fatx_cache_update_lru(struct inode *inode,
					struct fatx_cache *cache)
{
	if (FATX_I(inode)->cache_lru.next != &cache->cache_list)
		list_move(&cache->cache_list, &FATX_I(inode)->cache_lru);
}

static int fatx_cache_lookup(struct inode *inode, int fclus,
			    struct fatx_cache_id *cid,
			    int *cached_fclus, int *cached_dclus)
{
	static struct fatx_cache nohit = { .fcluster = 0, };

	struct fatx_cache *hit = &nohit, *p;
	int offset = -1;

	spin_lock(&FATX_I(inode)->cache_lru_lock);
	list_for_each_entry(p, &FATX_I(inode)->cache_lru, cache_list) {
		/* Find the cache of "fclus" or nearest cache. */
		if (p->fcluster <= fclus && hit->fcluster < p->fcluster) {
			hit = p;
			if ((hit->fcluster + hit->nr_contig) < fclus) {
				offset = hit->nr_contig;
			} else {
				offset = fclus - hit->fcluster;
				break;
			}
		}
	}
	if (hit != &nohit) {
		fatx_cache_update_lru(inode, hit);

		cid->id = FATX_I(inode)->cache_valid_id;
		cid->nr_contig = hit->nr_contig;
		cid->fcluster = hit->fcluster;
		cid->dcluster = hit->dcluster;
		*cached_fclus = cid->fcluster + offset;
		*cached_dclus = cid->dcluster + offset;
	}
	spin_unlock(&FATX_I(inode)->cache_lru_lock);

	return offset;
}

static struct fatx_cache *fatx_cache_merge(struct inode *inode,
					 struct fatx_cache_id *new)
{
	struct fatx_cache *p;

	list_for_each_entry(p, &FATX_I(inode)->cache_lru, cache_list) {
		/* Find the same part as "new" in cluster-chain. */
		if (p->fcluster == new->fcluster) {
			BUG_ON(p->dcluster != new->dcluster);
			if (new->nr_contig > p->nr_contig)
				p->nr_contig = new->nr_contig;
			return p;
		}
	}
	return NULL;
}

static void fatx_cache_add(struct inode *inode, struct fatx_cache_id *new)
{
	struct fatx_cache *cache, *tmp;

	if (new->fcluster == -1) /* dummy cache */
		return;

	spin_lock(&FATX_I(inode)->cache_lru_lock);
	if (new->id != FAT_CACHE_VALID &&
	    new->id != FATX_I(inode)->cache_valid_id)
		goto out;	/* this cache was invalidated */

	cache = fatx_cache_merge(inode, new);
	if (cache == NULL) {
		if (FATX_I(inode)->nr_caches < fatx_max_cache(inode)) {
			FATX_I(inode)->nr_caches++;
			spin_unlock(&FATX_I(inode)->cache_lru_lock);

			tmp = fatx_cache_alloc(inode);
			spin_lock(&FATX_I(inode)->cache_lru_lock);
			cache = fatx_cache_merge(inode, new);
			if (cache != NULL) {
				FATX_I(inode)->nr_caches--;
				fatx_cache_free(tmp);
				goto out_update_lru;
			}
			cache = tmp;
		} else {
			struct list_head *p = FATX_I(inode)->cache_lru.prev;
			cache = list_entry(p, struct fatx_cache, cache_list);
		}
		cache->fcluster = new->fcluster;
		cache->dcluster = new->dcluster;
		cache->nr_contig = new->nr_contig;
	}
out_update_lru:
	fatx_cache_update_lru(inode, cache);
out:
	spin_unlock(&FATX_I(inode)->cache_lru_lock);
}

/*
 * Cache invalidation occurs rarely, thus the LRU chain is not updated. It
 * fixes itself after a while.
 */
static void __fatx_cache_inval_inode(struct inode *inode)
{
	struct fatx_inode_info *i = FATX_I(inode);
	struct fatx_cache *cache;

	while (!list_empty(&i->cache_lru)) {
		cache = list_entry(i->cache_lru.next, struct fatx_cache, cache_list);
		list_del_init(&cache->cache_list);
		i->nr_caches--;
		fatx_cache_free(cache);
	}
	/* Update. The copy of caches before this id is discarded. */
	i->cache_valid_id++;
	if (i->cache_valid_id == FAT_CACHE_VALID)
		i->cache_valid_id++;
}

void fatx_cache_inval_inode(struct inode *inode)
{
	spin_lock(&FATX_I(inode)->cache_lru_lock);
	__fatx_cache_inval_inode(inode);
	spin_unlock(&FATX_I(inode)->cache_lru_lock);
}

static __s64 __fatx_access(struct super_block *sb, int nr, __s64 new_value)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct buffer_head *bh, *bh2, *c_bh, *c_bh2;
	unsigned char *p_first, *p_last;
	unsigned long first = 0, last = 0, next = 0;
	int b, copy;

	if (sbi->fatx_bits == 32) {
		first = last = nr*4;
	} else if (sbi->fatx_bits == 16) {
		first = last = nr*2;
	}
	b = sbi->fatx_start + (first >> sb->s_blocksize_bits);
	if (!(bh = sb_bread(sb, b))) {
		printk(KERN_ERR "FATX: bread(block %d) in"
		       " fatx_access failed\n", b);
		return -EIO;
	}
	if ((first >> sb->s_blocksize_bits) == (last >> sb->s_blocksize_bits)) {
		bh2 = bh;
	} else {
		if (!(bh2 = sb_bread(sb, b + 1))) {
			brelse(bh);
			printk(KERN_ERR "FATX: bread(block %d) in"
			       " fatx_access failed\n", b + 1);
			return -EIO;
		}
	}
	if (sbi->fatx_bits == 32) {
		p_first = p_last = NULL; /* GCC needs that stuff */
		next = le32_to_cpu(((__u32 *) bh->b_data)[(first &
		    (sb->s_blocksize - 1)) >> 2]);
//		next &= 0xffffffff;
	} else if (sbi->fatx_bits == 16) {
		p_first = p_last = NULL; /* GCC needs that stuff */
		next = le16_to_cpu(((__le16 *) bh->b_data)[(first &
		    (sb->s_blocksize - 1)) >> 1]);
	}
	PRINTK("FATX: fatx_access: 0x%x, nr=0x%x, first=0x%x, next=0x%x new_value=0x%x\n", 
			b, (int)nr, (int)first, (int)next, (int)new_value);
	if (new_value != -1) {
		if (sbi->fatx_bits == 32) {
			((__u32 *)bh->b_data)[(first & (sb->s_blocksize - 1)) >> 2]
				= cpu_to_le32(new_value);
		} else if (sbi->fatx_bits == 16) {
			((__le16 *)bh->b_data)[(first & (sb->s_blocksize - 1)) >> 1]
				= cpu_to_le16(new_value);
		}
		mark_buffer_dirty(bh);
		for (copy = 1; copy < sbi->fatxs; copy++) {
			b = sbi->fatx_start + (first >> sb->s_blocksize_bits)
				+ sbi->fatx_length * copy;
			if (!(c_bh = sb_bread(sb, b)))
				break;
			if (bh != bh2) {
				if (!(c_bh2 = sb_bread(sb, b+1))) {
					brelse(c_bh);
					break;
				}
				memcpy(c_bh2->b_data, bh2->b_data, sb->s_blocksize);
				mark_buffer_dirty(c_bh2);
				brelse(c_bh2);
			}
			memcpy(c_bh->b_data, bh->b_data, sb->s_blocksize);
			mark_buffer_dirty(c_bh);
			brelse(c_bh);
		}
	}
	brelse(bh);
	if (bh != bh2)
		brelse(bh2);
	return next;
}

/*
 * Returns the this'th FAT entry, -1 if it is an end-of-file entry. If
 * new_value is != -1, that FAT entry is replaced by it.
 */
__s64 fatx_access(struct super_block *sb, int nr, __s64 new_value)
{
	__s64 next;

	next = -EIO;
	if (nr < FAT_START_ENT || FATX_SB(sb)->max_cluster <= nr) {
		fatx_fs_panic(sb, "invalid access to FAT (entry 0x%08x)", nr);
		goto out;
	}

	if (new_value == FAT_ENT_EOF)
		new_value = EOF_FAT(sb);
	
	next = __fatx_access(sb, nr, new_value);
	if (next < 0)
		goto out;

	if (next >= BAD_FAT(sb))
		next = FAT_ENT_EOF;

out:
	return next;
}

static inline int cache_contiguous(struct fatx_cache_id *cid, int dclus)
{
	cid->nr_contig++;
	return ((cid->dcluster + cid->nr_contig) == dclus);
}

static inline void cache_init(struct fatx_cache_id *cid, int fclus, int dclus)
{
	cid->id = FAT_CACHE_VALID;
	cid->fcluster = fclus;
	cid->dcluster = dclus;
	cid->nr_contig = 0;
}

__s64 fatx_get_cluster(struct inode *inode, __s64 cluster, int *fclus, int *dclus)
{
	struct super_block *sb = inode->i_sb;
	//const int limit = sb->s_maxbytes >> FATX_SB(sb)->cluster_bits;
	const int limit = FATX_SB(sb)->max_cluster;
	struct fatx_cache_id cid;
	__s64 nr;

	PRINTK("FATX: %s \n", __FUNCTION__);
	BUG_ON(FATX_I(inode)->i_start == 0);

	*fclus = 0;
	*dclus = FATX_I(inode)->i_start;
	if (cluster == 0)
		return 0;

	if (fatx_cache_lookup(inode, cluster, &cid, fclus, dclus) < 0) {
		/*
		 * dummy, always not contiguous
		 * This is reinitialized by cache_init(), later.
		 */
		cache_init(&cid, -1, -1);
	}

	while (*fclus < cluster) {
		/* prevent the infinite loop of cluster chain */
		if (*fclus > limit) {
			fatx_fs_panic(sb, "%s: detected the cluster chain loop"
				     " (i_pos %lld)", __FUNCTION__,
				     FATX_I(inode)->i_pos);
			return -EIO;
		}

		nr = fatx_access(sb, *dclus, -1);
		if (nr < 0)
			return nr;
		else if (nr == FAT_ENT_FREE) {
			fatx_fs_panic(sb, "%s: invalid cluster chain"
				     " (i_pos %lld)", __FUNCTION__,
				     FATX_I(inode)->i_pos);
			return -EIO;
		} else if (nr == FAT_ENT_EOF) {
			fatx_cache_add(inode, &cid);
			return FAT_ENT_EOF;
		}
		(*fclus)++;
		*dclus = nr;
		if (!cache_contiguous(&cid, *dclus))
			cache_init(&cid, *fclus, *dclus);
	}
	fatx_cache_add(inode, &cid);
	return 0;
}

static __s64 fatx_bmap_cluster(struct inode *inode, __s64 cluster)
{
	struct super_block *sb = inode->i_sb;
	__s64 ret;
	int fclus, dclus;

	PRINTK("FATX: %s \n", __FUNCTION__);
	if (FATX_I(inode)->i_start == 0)
		return 0;

	ret = fatx_get_cluster(inode, cluster, &fclus, &dclus);
	if (ret < 0) {
		return ret;
	}
	else if (ret == FAT_ENT_EOF) {
		fatx_fs_panic(sb, "%s: request beyond EOF (i_pos %lld)",
			     __FUNCTION__, FATX_I(inode)->i_pos);
		return -EIO;
	}
	return dclus;
}

unsigned long fatx_bmap(struct inode *inode, sector_t sector, sector_t *phys)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	sector_t last_block;
	__s64 cluster, offset;

	PRINTK("FATX: %s \n", __FUNCTION__);
	*phys = 0;
	if ((inode->i_ino == FATX_ROOT_INO || (S_ISDIR(inode->i_mode) &&
	     !FATX_I(inode)->i_start))) {
		if (sector < (sbi->dir_entries >> sbi->dir_per_block_bits))
			*phys = sector + sbi->dir_start;
		PRINTK("FATX: %s phys %ld sector %ld\n",__FUNCTION__, *phys, sector);
		return 0;
	}
	last_block = (FATX_I(inode)->mmu_private + (sb->s_blocksize - 1))
		>> sb->s_blocksize_bits;
	if (sector >= last_block) {
		PRINTK("FATX: %s sector %ld last_block %ld\n", __FUNCTION__, sector, last_block);
		return 0;
	}

	cluster = sector >> (sbi->cluster_bits - sb->s_blocksize_bits);
	offset  = sector & (sbi->sec_per_clus - 1);
	cluster = fatx_bmap_cluster(inode, cluster);
	PRINTK("FATX: %s cluster %lld  offset %lld sector %ld\n",__FUNCTION__,cluster, offset, sector);
	if (cluster < 0)
		return cluster;
	else if (cluster)
		*phys = fatx_clus_to_blknr(sbi, cluster) + offset;
	return 0;
}
