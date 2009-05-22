/*
 * Copyright (C) 2004, OGAWA Hirofumi
 * Released under GPL v2.
 *
 *  FATX port 2005 by Edgar Hucek
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fatx_fs.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

struct fatxent_operations {
	void (*ent_blocknr)(struct super_block *, int, int *, sector_t *);
	void (*ent_set_ptr)(struct fatx_entry *, unsigned long);
	int (*ent_bread)(struct super_block *, struct fatx_entry *,
			 unsigned long, sector_t);
	unsigned long (*ent_get)(struct fatx_entry *);
	void (*ent_put)(struct fatx_entry *, unsigned long);
	int (*ent_next)(struct fatx_entry *);
};

static void fatx_ent_blocknr(struct super_block *sb, int entry,
			    int *offset, sector_t *blocknr)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	int bytes = (entry << sbi->fatxent_shift);
	//WARN_ON(entry < FAT_START_ENT || sbi->max_cluster <= entry);
	WARN_ON(sbi->max_cluster <= entry);
	*offset = bytes & (sb->s_blocksize - 1);
	*blocknr = sbi->fatx_start + (bytes >> sb->s_blocksize_bits);
}

static void fatx16_ent_set_ptr(struct fatx_entry *fatxent, unsigned long offset)
{
	WARN_ON(offset & (2 - 1));
	fatxent->u.ent16_p = (__le16 *)(fatxent->bhs[0]->b_data + offset);
}

static void fatx32_ent_set_ptr(struct fatx_entry *fatxent, unsigned long offset)
{
	WARN_ON(offset & (4 - 1));
	fatxent->u.ent32_p = (__u32 *)(fatxent->bhs[0]->b_data + offset);
}

static int fatx_ent_bread(struct super_block *sb, struct fatx_entry *fatxent,
			 unsigned long offset, sector_t blocknr)
{
	struct fatxent_operations *ops = FATX_SB(sb)->fatxent_ops;

	WARN_ON(blocknr < FATX_SB(sb)->fatx_start);
	fatxent->bhs[0] = sb_bread(sb, blocknr);
	if (!fatxent->bhs[0]) {
		printk(KERN_ERR "FATX: FAT read failed (blocknr %llu)\n",
		       (unsigned long long)blocknr);
		return -EIO;
	}
	fatxent->nr_bhs = 1;
	ops->ent_set_ptr(fatxent, offset);
	return 0;
}

static unsigned long fatx16_ent_get(struct fatx_entry *fatxent)
{
	unsigned long next = le16_to_cpu(*fatxent->u.ent16_p);
	WARN_ON((unsigned long)fatxent->u.ent16_p & (2 - 1));
	if (next >= BAD_FAT16)
		next = FAT_ENT_EOF;
	return next;
}

static unsigned long fatx32_ent_get(struct fatx_entry *fatxent)
{
	unsigned long next = le32_to_cpu(*fatxent->u.ent32_p);
	WARN_ON((unsigned long)fatxent->u.ent32_p & (4 - 1));
	if (next >= BAD_FAT32)
		next = FAT_ENT_EOF;
	return next;
}

static void fatx16_ent_put(struct fatx_entry *fatxent, unsigned long new)
{
	if (new == FAT_ENT_EOF)
		new = EOF_FAT16;

	//if (new == EOF_FAT16) new = 0xffff;
	PRINTK("FATX: %s new 0x%08lx\n", __FUNCTION__, new);
	*fatxent->u.ent16_p = cpu_to_le16(new);
	mark_buffer_dirty(fatxent->bhs[0]);
}

static void fatx32_ent_put(struct fatx_entry *fatxent, unsigned long new)
{
	if (new == FAT_ENT_EOF)
		new = EOF_FAT32;

	//if (new == EOF_FAT32) new = 0xffffffff;
	//new = le32_to_cpu(*fatxent->u.ent32_p);
	PRINTK("FATX: %s new 0x%08lx\n", __FUNCTION__, new);
	*fatxent->u.ent32_p = cpu_to_le32(new);
	mark_buffer_dirty(fatxent->bhs[0]);
}

static int fatx16_ent_next(struct fatx_entry *fatxent)
{
	const struct buffer_head *bh = fatxent->bhs[0];
	fatxent->entry++;
	if (fatxent->u.ent16_p < (__le16 *)(bh->b_data + (bh->b_size - 2))) {
		fatxent->u.ent16_p++;
		return 1;
	}
	fatxent->u.ent16_p = NULL;
	return 0;
}

static int fatx32_ent_next(struct fatx_entry *fatxent)
{
	const struct buffer_head *bh = fatxent->bhs[0];
	fatxent->entry++;
	if (fatxent->u.ent32_p < (__u32 *)(bh->b_data + (bh->b_size - 4))) {
		fatxent->u.ent32_p++;
		return 1;
	}
	fatxent->u.ent32_p = NULL;
	return 0;
}

static struct fatxent_operations fatx16_ops = {
	.ent_blocknr	= fatx_ent_blocknr,
	.ent_set_ptr	= fatx16_ent_set_ptr,
	.ent_bread	= fatx_ent_bread,
	.ent_get	= fatx16_ent_get,
	.ent_put	= fatx16_ent_put,
	.ent_next	= fatx16_ent_next,
};

static struct fatxent_operations fatx32_ops = {
	.ent_blocknr	= fatx_ent_blocknr,
	.ent_set_ptr	= fatx32_ent_set_ptr,
	.ent_bread	= fatx_ent_bread,
	.ent_get	= fatx32_ent_get,
	.ent_put	= fatx32_ent_put,
	.ent_next	= fatx32_ent_next,
};

static inline void lock_fatx(struct fatx_sb_info *sbi)
{
	down(&sbi->fatx_lock);
}

static inline void unlock_fatx(struct fatx_sb_info *sbi)
{
	up(&sbi->fatx_lock);
}

void fatx_ent_access_init(struct super_block *sb)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);

	init_MUTEX(&sbi->fatx_lock);

	switch (sbi->fatx_bits) {
	case 32:
		sbi->fatxent_shift = 2;
		sbi->fatxent_ops = &fatx32_ops;
		break;
	case 16:
		sbi->fatxent_shift = 1;
		sbi->fatxent_ops = &fatx16_ops;
		break;
	}
}

static inline int fatx_ent_update_ptr(struct super_block *sb,
				     struct fatx_entry *fatxent,
				     int offset, sector_t blocknr)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct fatxent_operations *ops = sbi->fatxent_ops;
	struct buffer_head **bhs = fatxent->bhs;

	/* Is this fatxent's blocks including this entry? */
	if (!fatxent->nr_bhs || bhs[0]->b_blocknr != blocknr)
		return 0;
	ops->ent_set_ptr(fatxent, offset);
	return 1;
}

__s64 fatx_ent_read(struct inode *inode, struct fatx_entry *fatxent, __s64 entry)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	struct fatxent_operations *ops = sbi->fatxent_ops;
	__s64 err; 
	int offset;
	sector_t blocknr;

	if (entry < FAT_START_ENT || sbi->max_cluster <= entry) {
		fatxent_brelse(fatxent);
		fatx_fs_panic(sb, "invalid access to FAT (entry 0x%08x)", entry);
		return -EIO;
	}

	fatxent_set_entry(fatxent, entry);
	ops->ent_blocknr(sb, entry, &offset, &blocknr);

	if (!fatx_ent_update_ptr(sb, fatxent, offset, blocknr)) {
		fatxent_brelse(fatxent);
		err = ops->ent_bread(sb, fatxent, offset, blocknr);
		if (err)
			return err;
	}
	err = ops->ent_get(fatxent);
	PRINTK("FATX: %s ent_get err 0x%08llx entry 0x%08llx\n", __FUNCTION__, err, entry);
	return err;
}

/* FIXME: We can write the blocks as more big chunk. */
static int fatx_mirror_bhs(struct super_block *sb, struct buffer_head **bhs,
			  int nr_bhs)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct buffer_head *c_bh;
	int err, n, copy;

	err = 0;
	for (copy = 1; copy < sbi->fatxs; copy++) {
		sector_t backup_fatx = sbi->fatx_length * copy;

		for (n = 0; n < nr_bhs; n++) {
			c_bh = sb_getblk(sb, backup_fatx + bhs[n]->b_blocknr);
			if (!c_bh) {
				err = -ENOMEM;
				goto error;
			}
			memcpy(c_bh->b_data, bhs[n]->b_data, sb->s_blocksize);
			set_buffer_uptodate(c_bh);
			mark_buffer_dirty(c_bh);
			if (sb->s_flags & MS_SYNCHRONOUS)
				err = sync_dirty_buffer(c_bh);
			brelse(c_bh);
			if (err)
				goto error;
		}
	}
error:
	return err;
}

int fatx_ent_write(struct inode *inode, struct fatx_entry *fatxent,
		  __s64 new, int wait)
{
	struct super_block *sb = inode->i_sb;
	struct fatxent_operations *ops = FATX_SB(sb)->fatxent_ops;
	int err;

	ops->ent_put(fatxent, new);
	if (wait) {
		err = fatx_sync_bhs(fatxent->bhs, fatxent->nr_bhs);
		if (err)
			return err;
	}
	return fatx_mirror_bhs(sb, fatxent->bhs, fatxent->nr_bhs);
}

static inline int fatx_ent_next(struct fatx_sb_info *sbi,
			       struct fatx_entry *fatxent)
{
	if (sbi->fatxent_ops->ent_next(fatxent)) {
		if (fatxent->entry < sbi->max_cluster)
			return 1;
	}
	return 0;
}

static inline int fatx_ent_read_block(struct super_block *sb,
				     struct fatx_entry *fatxent)
{
	struct fatxent_operations *ops = FATX_SB(sb)->fatxent_ops;
	sector_t blocknr;
	int offset;

	fatxent_brelse(fatxent);
	ops->ent_blocknr(sb, fatxent->entry, &offset, &blocknr);
	return ops->ent_bread(sb, fatxent, offset, blocknr);
}

static void fatx_collect_bhs(struct buffer_head **bhs, int *nr_bhs,
			    struct fatx_entry *fatxent)
{
	int n, i;

	for (n = 0; n < fatxent->nr_bhs; n++) {
		for (i = 0; i < *nr_bhs; i++) {
			if (fatxent->bhs[n] == bhs[i])
				break;
		}
		if (i == *nr_bhs) {
			get_bh(fatxent->bhs[n]);
			bhs[i] = fatxent->bhs[n];
			(*nr_bhs)++;
		}
	}
}

__s64 fatx_alloc_clusters(struct inode *inode, __s64 *cluster, int nr_cluster)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct fatxent_operations *ops = sbi->fatxent_ops;
	struct fatx_entry fatxent, prev_ent;
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	int i, count,  nr_bhs, idx_clus;
	__s64 err;

	BUG_ON(nr_cluster > (MAX_BUF_PER_PAGE / 2));	/* fixed limit */

	lock_fatx(sbi);
	if (sbi->free_clusters != -1 && sbi->free_clusters < nr_cluster) {
		unlock_fatx(sbi);
		return -ENOSPC;
	}

	err = nr_bhs = idx_clus = 0;
	count = FAT_START_ENT;
	fatxent_init(&prev_ent);
	fatxent_init(&fatxent);
	fatxent_set_entry(&fatxent, sbi->prev_free + 1);
	while (count < sbi->max_cluster) {
		if (fatxent.entry >= sbi->max_cluster)
			fatxent.entry = FAT_START_ENT;
		fatxent_set_entry(&fatxent, fatxent.entry);
		err = fatx_ent_read_block(sb, &fatxent);
		if (err)
			goto out;

		/* Find the free entries in a block */
		do {
			if (ops->ent_get(&fatxent) == FAT_ENT_FREE) {
				int entry = fatxent.entry;

				/* make the cluster chain */
				ops->ent_put(&fatxent, FAT_ENT_EOF);
				if (prev_ent.nr_bhs)
					ops->ent_put(&prev_ent, entry);

				fatx_collect_bhs(bhs, &nr_bhs, &fatxent);

				sbi->prev_free = entry;
				if (sbi->free_clusters != -1)
					sbi->free_clusters--;

				cluster[idx_clus] = entry;
				idx_clus++;
				if (idx_clus == nr_cluster)
					goto out;

				/*
				 * fatx_collect_bhs() gets ref-count of bhs,
				 * so we can still use the prev_ent.
				 */
				prev_ent = fatxent;
			}
			count++;
			if (count == sbi->max_cluster)
				break;
		} while (fatx_ent_next(sbi, &fatxent));
	}

	/* Couldn't allocate the free entries */
	sbi->free_clusters = 0;
	err = -ENOSPC;

out:

	unlock_fatx(sbi);
	fatxent_brelse(&fatxent);
	if (!err) {
		PRINTK("FATX: %s cluster %lld err %lld\n", __FUNCTION__, cluster[0], err);
		if (inode_needs_sync(inode))
			err = fatx_sync_bhs(bhs, nr_bhs);
		if (!err)
			err = fatx_mirror_bhs(sb, bhs, nr_bhs);
	}
	for (i = 0; i < nr_bhs; i++)
		brelse(bhs[i]);

	if (err && idx_clus)
		fatx_free_clusters(inode, cluster[0]);

	return err;
}

__s64 fatx_free_clusters(struct inode *inode, __s64 cluster)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct fatxent_operations *ops = sbi->fatxent_ops;
	struct fatx_entry fatxent;
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	int i, nr_bhs;
	__s64 err; 
	
	nr_bhs = 0;
	fatxent_init(&fatxent);
	lock_fatx(sbi);
	do {
		PRINTK("FATX: %s cluster %lld\n", __FUNCTION__, cluster);
		cluster = fatx_ent_read(inode, &fatxent, cluster);
		if (cluster < 0) {
			err = cluster;
			goto error;
		} else if (cluster == FAT_ENT_FREE) {
			fatx_fs_panic(sb, "%s: deleting FAT entry beyond EOF",
				     __FUNCTION__);
			err = -EIO;
			goto error;
		}

		ops->ent_put(&fatxent, FAT_ENT_FREE);
		if (sbi->free_clusters != -1)
			sbi->free_clusters++;

		if (nr_bhs + fatxent.nr_bhs > MAX_BUF_PER_PAGE) {
			if (sb->s_flags & MS_SYNCHRONOUS) {
				err = fatx_sync_bhs(bhs, nr_bhs);
				if (err)
					goto error;
			}
			err = fatx_mirror_bhs(sb, bhs, nr_bhs);
			if (err)
				goto error;
			for (i = 0; i < nr_bhs; i++)
				brelse(bhs[i]);
			nr_bhs = 0;
		}
		fatx_collect_bhs(bhs, &nr_bhs, &fatxent);
	} while (cluster != FAT_ENT_EOF);

	if (sb->s_flags & MS_SYNCHRONOUS) {
		err = fatx_sync_bhs(bhs, nr_bhs);
		if (err)
			goto error;
	}
	err = fatx_mirror_bhs(sb, bhs, nr_bhs);
error:
	fatxent_brelse(&fatxent);
	for (i = 0; i < nr_bhs; i++)
		brelse(bhs[i]);
	unlock_fatx(sbi);

	return err;
}

EXPORT_SYMBOL(fatx_free_clusters);

__s64 fatx_count_free_clusters(struct super_block *sb)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct fatxent_operations *ops = sbi->fatxent_ops;
	struct fatx_entry fatxent;
	__s64 err = 0;
	int free;

	lock_fatx(sbi);
	if (sbi->free_clusters != -1)
		goto out;

	free = 0;
	fatxent_init(&fatxent);
	fatxent_set_entry(&fatxent, FAT_START_ENT);
	while (fatxent.entry < sbi->max_cluster) {
		err = fatx_ent_read_block(sb, &fatxent);
		if (err)
			goto out;

		do {
			if (ops->ent_get(&fatxent) == FAT_ENT_FREE)
				free++;
		} while (fatx_ent_next(sbi, &fatxent));
	}
	sbi->free_clusters = free;
	fatxent_brelse(&fatxent);
out:
	unlock_fatx(sbi);
	return err;
}
