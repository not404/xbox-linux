/*
 *  linux/fs/fatx/dir.c
 *
 *  directory handling functions for fatx-based filesystems
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  Hidden files 1995 by Albert Cahalan <albert@ccs.neu.edu> <adc@coe.neu.edu>
 *
 *  VFAT extensions by Gordon Chaffee <chaffee@plateau.cs.berkeley.edu>
 *  Merged with fatx fs by Henrik Storner <storner@osiris.ping.dk>
 *  Rewritten for constant inumbers. Plugged buffer overrun in readdir(). AV
 *  Short name translation 1999, 2001 by Wolfram Pienkoss <wp@bszh.de>
 *
 *  FATX port 2005 by Edgar Hucek
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/fatx_fs.h>
#include <linux/dirent.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>
#include <asm/uaccess.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

static inline loff_t fatx_make_i_pos(struct super_block *sb,
				    struct buffer_head *bh,
				    struct fatx_dir_entry *de)
{
	return ((loff_t)bh->b_blocknr << FATX_SB(sb)->dir_per_block_bits)
		| (de - (struct fatx_dir_entry *)bh->b_data);
}

/* Returns the inode number of the directory entry at offset pos. If bh is
   non-NULL, it is brelse'd before. Pos is incremented. The buffer header is
   returned in bh.
   AV. Most often we do it item-by-item. Makes sense to optimize.
   AV. OK, there we go: if both bh and de are non-NULL we assume that we just
   AV. want the next entry (took one explicit de=NULL in vfatx/namei.c).
   AV. It's done in fatx_get_entry() (inlined), here the slow case lives.
   AV. Additionally, when we return -1 (i.e. reached the end of directory)
   AV. we make bh NULL.
 */
static int fatx__get_entry(struct inode *dir, loff_t *pos,
			  struct buffer_head **bh, struct fatx_dir_entry **de)
{
	struct super_block *sb = dir->i_sb;
	sector_t phys, iblock;
	int offset;
	int err;

next:
	if (*bh)
		brelse(*bh);

	*bh = NULL;
	iblock = *pos >> sb->s_blocksize_bits;
	err = fatx_bmap(dir, iblock, &phys);
	if (err || !phys) {
		PRINTK("FATX: %s fatx_bmap err %d phys %ld iblock %ld\n", 
			__FUNCTION__, err, phys, iblock);
		return -1;	/* beyond EOF or error */
	}

	*bh = sb_bread(sb, phys);
	if (*bh == NULL) {
		printk(KERN_ERR "FATX: Directory bread(block %llu) failed\n",
		       (unsigned long long)phys);
		/* skip this block */
		*pos = (iblock + 1) << sb->s_blocksize_bits;
		goto next;
	}

	offset = *pos & (sb->s_blocksize - 1);
	*pos += sizeof(struct fatx_dir_entry);
	*de = (struct fatx_dir_entry *)((*bh)->b_data + offset);

	return 0;
}

static inline int fatx_get_entry(struct inode *dir, loff_t *pos,
				struct buffer_head **bh,
				struct fatx_dir_entry **de)
{
	/* Fast stuff first */
	if (*bh && *de &&
	    (*de - (struct fatx_dir_entry *)(*bh)->b_data) < FATX_SB(dir->i_sb)->dir_per_block - 1) {
		*pos += sizeof(struct fatx_dir_entry);
		(*de)++;
		return 0;
	}
	return fatx__get_entry(dir, pos, bh, de);
}

static int fatx_readdir(struct file *filp, void *dirent,
			filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	/*
	unsigned long dummy;
	*/
	unsigned long lpos, *furrfu = &lpos;
	unsigned long inum;
	loff_t cpos;
	int ret = 0;
	loff_t i_pos;
	struct inode *tmp;

	lock_kernel();

	PRINTK("FATX: %s\n", __FUNCTION__ );
	cpos = filp->f_pos;
	/* Fake . and .. for the root directory. */
	/*
	if (inode->i_ino == FATX_ROOT_INO) {
		while (cpos < 2) {
			if (filldir(dirent, "..", cpos+1, cpos, FATX_ROOT_INO, DT_DIR) < 0)
				goto out;
			cpos++;
			filp->f_pos++;
		}
		if (cpos == 2) {
			dummy = 2;
			furrfu = &dummy;
			cpos = 0;
		}
	}
	*/
	if (cpos & (sizeof(struct fatx_dir_entry)-1)) {
		ret = -ENOENT;
		goto out;
	}

	bh = NULL;
GetNew:
	if (fatx_get_entry(inode, &cpos, &bh, &de) == -1)
		goto EODir;
	/* Check for long filename entry */
	if (FATX_IS_FREE(de))
		goto RecEnd;

	lpos = cpos - sizeof(struct fatx_dir_entry);
	i_pos = fatx_make_i_pos(sb, bh, de);
	tmp = fatx_iget(sb, i_pos);
	if (tmp) {
		inum = tmp->i_ino;
		iput(tmp);
	} else
		inum = iunique(sb, FATX_ROOT_INO);

	if (filldir(dirent, de->name, de->name_length, *furrfu, inum,
		    (de->attr & ATTR_DIR) ? DT_DIR : DT_REG) < 0)
		goto FillFailed;

RecEnd:
	furrfu = &lpos;
	filp->f_pos = cpos;
	goto GetNew;
EODir:
	filp->f_pos = cpos;
FillFailed:
	if (bh)
		brelse(bh);
out:
	unlock_kernel();
	return ret;
}

struct file_operations fatx_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= fatx_readdir,
	.ioctl		= NULL,
	.fsync		= file_fsync,
};

static int fatx_get_short_entry(struct inode *dir, loff_t *pos,
			       struct buffer_head **bh,
			       struct fatx_dir_entry **de)
{
	while (fatx_get_entry(dir, pos, bh, de) >= 0) {
		if (!FATX_IS_FREE(*de))
			return 0;
	}
	return -ENOENT;
}

/*
 * The ".." entry can not provide the "struct fat_slot_info" informations
 * for inode. So, this function provide the some informations only.
 */
/*
int fatx_get_dotdot_entry(struct inode *dir, struct buffer_head **bh,
			 struct fatx_dir_entry **de, loff_t *i_pos)
{
	loff_t offset;

	offset = 0;
	*bh = NULL;
	while (fatx_get_short_entry(dir, &offset, bh, de) >= 0) {
		if (!strncmp((*de)->name, "..", 2)) {
			*i_pos = fatx_make_i_pos(dir->i_sb, *bh, *de);
			return 0;
		}
	}
	return -ENOENT;
}

EXPORT_SYMBOL(fatx_get_dotdot_entry);
*/

/* See if directory is empty */
int fatx_dir_empty(struct inode *dir)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	loff_t cpos;
	int result = 0;

	bh = NULL;
	cpos = 0;
	while (fatx_get_entry(dir, &cpos, &bh, &de) >= 0) {
		if (!FATX_IS_FREE(de)) {
			result = -ENOTEMPTY;
			break;
		}
	}
	brelse(bh);
	return result;
}

EXPORT_SYMBOL(fatx_dir_empty);

/*
 * fatx_subdirs counts the number of sub-directories of dir. It can be run
 * on directories being created.
 */
int fatx_subdirs(struct inode *dir)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	loff_t cpos;
	int count = 0;

	bh = NULL;
	cpos = 0;
	while (fatx_get_short_entry(dir, &cpos, &bh, &de) >= 0) {
		if (de->attr & ATTR_DIR)
			count++;
	}
	brelse(bh);
	return count;
}

/*
 * Scans a directory for a given file (name points to its formatted name).
 * Returns an error code or zero.
 */
int fatx_scan(struct inode *dir, const unsigned char *name,
	     struct fatx_slot_info *sinfo)
{
	struct super_block *sb = dir->i_sb;

	sinfo->slot_off = 0;
	sinfo->bh = NULL;
	while (fatx_get_short_entry(dir, &sinfo->slot_off, &sinfo->bh,
				   &sinfo->de) >= 0) {
		if (!strncmp(sinfo->de->name, name, FATX_NAME)) {
			sinfo->slot_off -= sizeof(*sinfo->de);
			sinfo->i_pos = fatx_make_i_pos(sb, sinfo->bh, sinfo->de);
			return 0;
		}
	}
	return -ENOENT;
}

EXPORT_SYMBOL(fatx_scan);

static int __fatx_remove_entries(struct inode *dir, loff_t pos)
{
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh;
	struct fatx_dir_entry *de, *endp;
	int err = 0, nr_slots;

	bh = NULL;
	if (fatx_get_entry(dir, &pos, &bh, &de) < 0) {
		err = -EIO;
	}

	endp = (struct fatx_dir_entry *)(bh->b_data + sb->s_blocksize);
	de->name_length = DELETED_FLAG;
	de++;
	nr_slots--;
	mark_buffer_dirty(bh);
	if (IS_DIRSYNC(dir))
		err = sync_dirty_buffer(bh);
	brelse(bh);

	return err;
}

int fatx_remove_entries(struct inode *dir, struct fatx_slot_info *sinfo)
{
	struct fatx_dir_entry *de;
	struct buffer_head *bh;
	int err = 0;

	/*
	 * First stage: Remove the shortname. By this, the directory
	 * entry is removed.
	 */
	de = sinfo->de;
	sinfo->de = NULL;
	bh = sinfo->bh;
	sinfo->bh = NULL;
	de->name_length = DELETED_FLAG;
	de--;
	mark_buffer_dirty(bh);
	if (IS_DIRSYNC(dir))
		err = sync_dirty_buffer(bh);
	brelse(bh);
	if (err)
		return err;
	dir->i_version++;

	dir->i_mtime = dir->i_atime = CURRENT_TIME;
	if (IS_DIRSYNC(dir))
		(void)fatx_sync_inode(dir);
	else
		mark_inode_dirty(dir);

	return 0;
}

EXPORT_SYMBOL(fatx_remove_entries);

static int fatx_zeroed_cluster(struct inode *dir, sector_t blknr, int nr_used,
			      struct buffer_head **bhs, int nr_bhs)
{
	struct super_block *sb = dir->i_sb;
	sector_t last_blknr = blknr + FATX_SB(sb)->sec_per_clus;
	int err, i, n;

	/* Zeroing the unused blockson this cluster */
	blknr += nr_used;
	n = nr_used;
	while (blknr < last_blknr) {
		bhs[n] = sb_getblk(sb, blknr);
		if (!bhs[n]) {
			err = -ENOMEM;
			goto error;
		}
		memset(bhs[n]->b_data, 0xFF, sb->s_blocksize);
		set_buffer_uptodate(bhs[n]);
		mark_buffer_dirty(bhs[n]);

		n++;
		blknr++;
		if (n == nr_bhs) {
			if (IS_DIRSYNC(dir)) {
				err = fatx_sync_bhs(bhs, n);
				if (err)
					goto error;
			}
			for (i = 0; i < n; i++)
				brelse(bhs[i]);
			n = 0;
		}
	}
	if (IS_DIRSYNC(dir)) {
		err = fatx_sync_bhs(bhs, n);
		if (err)
			goto error;
	}
	for (i = 0; i < n; i++)
		brelse(bhs[i]);

	return 0;

error:
	for (i = 0; i < n; i++)
		bforget(bhs[i]);
	return err;
}

int fatx_alloc_new_dir(struct inode *dir, struct timespec *ts)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	struct fatx_dir_entry *de;
	sector_t blknr;
	__le16 date, time;
	__s64 err, cluster;

	err = fatx_alloc_clusters(dir, &cluster, 1);
	if (err) {
		PRINTK("FATX: %s fatx_alloc_clusters failed\n", __FUNCTION__);
		goto error;
	}

	blknr = fatx_clus_to_blknr(sbi, cluster);
	bhs[0] = sb_getblk(sb, blknr);
	if (!bhs[0]) {
		err = -ENOMEM;
		goto error_free;
	}

	fatx_date_unix2dos(ts->tv_sec, &time, &date);

	de = (struct fatx_dir_entry *)bhs[0]->b_data;
	/* filling the new directory slots ("." and ".." entries) */
	de->attr = de[1].attr = ATTR_DIR;
	de[0].name_length = 0xFF; //end of dir marker
	de[0].start = cpu_to_le32(cluster);
	de[1].start = cpu_to_le32(FATX_I(dir)->i_logstart);
	de[0].time = de[1].time = time;
	de[0].date = de[1].date = date;
	de[0].ctime = de[1].ctime = 0;
	de[0].adate = de[0].cdate = de[1].adate = de[1].cdate = 0;
	de[0].size = de[1].size = 0;
	memset(de, 0xFF, sb->s_blocksize);
	set_buffer_uptodate(bhs[0]);
	mark_buffer_dirty(bhs[0]);

	err = fatx_zeroed_cluster(dir, blknr, 1, bhs, MAX_BUF_PER_PAGE);
	if (err) {
		PRINTK("FATX: %s fatx_zeroed_cluster failed\n", __FUNCTION__);
		goto error_free;
	}

	return cluster;

error_free:
	fatx_free_clusters(dir, cluster);
error:
	return err;
}

EXPORT_SYMBOL(fatx_alloc_new_dir);

static int fatx_add_new_entries(struct inode *dir, void *slots, int *nr_cluster, 
			       struct fatx_dir_entry **de,
			       struct buffer_head **bh, loff_t *i_pos)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct buffer_head *bhs[MAX_BUF_PER_PAGE];
	sector_t blknr, start_blknr, last_blknr;
	unsigned long size, copy;
	int i, n, offset;
	__s64 err, cluster[2];

	PRINTK("FATX: %s\n", __FUNCTION__);
	/*
	 * The minimum cluster size is 512bytes, and maximum entry
	 * size is 32*slots (672bytes).  So, iff the cluster size is
	 * 512bytes, we may need two clusters.
	 */
	size = sizeof(struct fatx_dir_entry);
	*nr_cluster = (size + (sbi->cluster_size - 1)) >> sbi->cluster_bits;
	BUG_ON(*nr_cluster > 2);

	err = fatx_alloc_clusters(dir, cluster, *nr_cluster);
	PRINTK("FATX: %s fatx_alloc_clusters cluster %lld nr_cluster %d\n", 
			__FUNCTION__, cluster[0], *nr_cluster);
	if (err) {
		goto error;
	}

	/*
	 * First stage: Fill the directory entry.  NOTE: This cluster
	 * is not referenced from any inode yet, so updates order is
	 * not important.
	 */
	i = n = copy = 0;
	do {
		start_blknr = blknr = fatx_clus_to_blknr(sbi, cluster[i]);
		last_blknr = start_blknr + sbi->sec_per_clus;
		while (blknr < last_blknr) {
			bhs[n] = sb_getblk(sb, blknr);
			if (!bhs[n]) {
				err = -ENOMEM;
				goto error_nomem;
			}

			/* fill the directory entry */
			copy = min(size, sb->s_blocksize);
			memcpy(bhs[n]->b_data, slots, copy);
			slots += copy;
			size -= copy;
			set_buffer_uptodate(bhs[n]);
			mark_buffer_dirty(bhs[n]);
			if (!size)
				break;
			n++;
			blknr++;
		}
	} while (++i < *nr_cluster);

	memset(bhs[n]->b_data + copy, 0xFF, sb->s_blocksize - copy);
	offset = copy - sizeof(struct fatx_dir_entry);
	get_bh(bhs[n]);
	*bh = bhs[n];
	*de = (struct fatx_dir_entry *)((*bh)->b_data + offset);
	*i_pos = fatx_make_i_pos(sb, *bh, *de);

	/* Second stage: clear the rest of cluster, and write outs */
	err = fatx_zeroed_cluster(dir, start_blknr, ++n, bhs, MAX_BUF_PER_PAGE);
	if (err)
		goto error_free;

	return cluster[0];

error_free:
	brelse(*bh);
	*bh = NULL;
	n = 0;
error_nomem:
	for (i = 0; i < n; i++)
		bforget(bhs[i]);
	fatx_free_clusters(dir, cluster[0]);
error:
	return err;
}

int fatx_add_entries(struct inode *dir, void *slots, 
		     struct fatx_slot_info *sinfo)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct buffer_head *bh, *prev;
	struct fatx_dir_entry *de;
	__s64 err;
	loff_t pos, i_pos;
	int size, free_slots, nr_slots, cluster, nr_cluster, copy;
	loff_t offset;
	
	struct fatx_dir_entry *de1 = (struct fatx_dir_entry *)slots;
	PRINTK("FATX: %s entryname : ", __FUNCTION__ );
	if(fatx_debug) fatx_printname(de1->name, de1->name_length);
	PRINTK("\n");

	/* First stage: search free direcotry entries */
	free_slots = 0;
	nr_slots = 1;
	bh = prev = NULL;
	offset = pos = 0;
	err = -ENOSPC;
	while (fatx_get_entry(dir, &pos, &bh, &de) > -1) {
		// Root dir can only have 256 entries
		if(pos >= FATX_MAX_DIR_SIZE && dir->i_ino == FATX_ROOT_INO)
			goto error;
		if (FATX_IS_FREE(de)) {
			if (prev != bh) {
				get_bh(bh);
				prev = bh;
			}
			free_slots++;
			goto found;
		} else {
//			brelse(bh);
			prev = NULL;
			free_slots =  0;
		}
	}
	if (dir->i_ino == FATX_ROOT_INO) {
		err = -ENOSPC;
		goto error;
	}  else if (FATX_I(dir)->i_start == 0) {
		printk(KERN_ERR "FATX: Corrupted directory (i_pos %lld)\n",
		       FATX_I(dir)->i_pos);
		err = -EIO;
		goto error;
	}
found:
	err = 0;
	pos -= sizeof(*de);
	nr_slots -= free_slots;
	if(free_slots) {
		size = sizeof(*de);
		offset = pos & (sb->s_blocksize - 1);
		copy = min_t(int, sb->s_blocksize - offset, size);

		PRINTK("FATX: %s pos %lld size %d offset %lld copy %d\n", __FUNCTION__, pos, size, offset, copy);
		memcpy(bh->b_data + offset, slots, copy);
		mark_buffer_dirty(bh);
		if (IS_DIRSYNC(dir))
			err = sync_dirty_buffer(bh);
		if(err)
			goto error_remove;
		brelse(bh);
	}

	if(nr_slots) {
		/*
		 * Third stage: allocate the cluster for new entries.
		 * And initialize the cluster with new entries, then
		 * add the cluster to dir.
		 */
		cluster = fatx_add_new_entries(dir, slots, &nr_cluster, &de, &bh, &i_pos);
		if (cluster < 0) {
			err = cluster;
			goto error;
		}
		PRINTK("FATX: %s fatx_chain_add cluster %d nr_cluster %d\n", __FUNCTION__, cluster, nr_cluster);
		err = fatx_chain_add(dir, cluster, nr_cluster);
		if (err) {
			fatx_free_clusters(dir, cluster);
			goto error;
		}
		if (dir->i_size & (sbi->cluster_size - 1)) {
			fatx_fs_panic(sb, "Odd directory size");
			dir->i_size = (dir->i_size + sbi->cluster_size - 1)
				& ~((loff_t)sbi->cluster_size - 1);
		}
		dir->i_size += nr_cluster << sbi->cluster_bits;
		FATX_I(dir)->mmu_private += nr_cluster << sbi->cluster_bits;
	}
	sinfo->slot_off = pos;
	sinfo->de = de;
	sinfo->bh = bh;
	sinfo->i_pos = fatx_make_i_pos(sb, sinfo->bh, sinfo->de);

	return 0;

error:
	brelse(bh);
	return err;

error_remove:
	brelse(bh);
	if(free_slots)
		__fatx_remove_entries(dir, pos);
	return err;
}

EXPORT_SYMBOL(fatx_add_entries);
