/*
 *  linux/fs/fatx/dir.c
 *
 *  directory handling functions for fatx-based filesystems
 *
 *  Written 1992,1993 by Werner Almesberger
 *  FATX port 2005 by Edgar Hucek
 *
 *  Hidden files 1995 by Albert Cahalan <albert@ccs.neu.edu> <adc@coe.neu.edu>
 *
 *  VFAT extensions by Gordon Chaffee <chaffee@plateau.cs.berkeley.edu>
 *  Merged with fatx fs by Henrik Storner <storner@osiris.ping.dk>
 *  Rewritten for constant inumbers. Plugged buffer overrun in readdir(). AV
 *  Short name translation 1999, 2001 by Wolfram Pienkoss <wp@bszh.de>
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

static inline void fatx_printname(const char *name, int length)
{
	int i;
	for(i=0;i<length;i++) {
		PRINTK("%c",name[i]);
	}
	PRINTK("  len %d",length);
}

static int fatx_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	unsigned long lpos, dummy, *furrfu = &lpos;
	unsigned long inum;
	loff_t i_pos, cpos;
	int ret = 0;
	struct inode *tmp;

	PRINTK("FATX: %s \n", __FUNCTION__);
	lock_kernel();

	cpos = filp->f_pos;

	if(inode->i_ino == FATX_ROOT_INO) {
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

	if (cpos & (sizeof(struct fatx_dir_entry)-1)) {
		ret = -ENOENT;
		goto out;
	}

	bh = NULL;
GetNew:
	if (fatx_get_entry(inode,&cpos,&bh,&de,&i_pos) == -1) {
		goto EODir;
	}

	if (FATX_IS_FREE(de)) {
		goto RecEnd;
	}

	if (FATX_END_OF_DIR(de)) {
		goto RecEnd;
	}

	lpos = cpos - sizeof(struct fatx_dir_entry);	
	tmp = fatx_iget(sb, i_pos);
	if (tmp) {
		inum = tmp->i_ino;
		iput(tmp);
	} else
		inum = iunique(sb, FATX_ROOT_INO);

	if (filldir(dirent, de->name, de->name_length, *furrfu, inum,
		    (de->attr & ATTR_DIR) ? DT_DIR : DT_REG) < 0) {
		goto FillFailed;
	}

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
			       struct fatx_dir_entry **de, loff_t *i_pos)
{
	PRINTK("FATX: %s \n", __FUNCTION__);
	while (fatx_get_entry(dir, pos, bh, de, i_pos) >= 0) {
		/* free entry or long name entry or volume label */
		if (!FATX_IS_FREE(*de))
			return 0;
	}
	return -ENOENT;
}

/* See if directory is empty */
int fatx_dir_empty(struct inode *dir)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	loff_t cpos, i_pos;
	int result = 0;

	PRINTK("FATX: %s \n", __FUNCTION__);
	bh = NULL;
	cpos = 0;
	while (fatx_get_entry(dir, &cpos, &bh, &de, &i_pos) >= 0) {
		if (FATX_END_OF_DIR(de)) {
			break;
		}
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
	loff_t cpos, i_pos;
	int count = 0;

	PRINTK("FATX: %s \n", __FUNCTION__);
	bh = NULL;
	cpos = 0;
	while (fatx_get_short_entry(dir, &cpos, &bh, &de, &i_pos) >= 0) {
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
	     struct buffer_head **bh, struct fatx_dir_entry **de,
	     loff_t *i_pos)
{
	loff_t cpos;

	PRINTK("FATX: %s \n", __FUNCTION__);
	*bh = NULL;
	cpos = 0;
	while (fatx_get_short_entry(dir, &cpos, bh, de, i_pos) >= 0) {
		if (!strncmp((*de)->name, name, FATX_NAME)) {
			return 0;
		}
	}
	return -ENOENT;
}

EXPORT_SYMBOL(fatx_scan);

static struct buffer_head *fatx_extend_dir(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh, *res = NULL;
	__s64 nr; 
	int sec_per_clus = FATX_SB(sb)->sec_per_clus;
	sector_t sector, last_sector;

	PRINTK("FATX: %s \n", __FUNCTION__);
	if (inode->i_ino == FATX_ROOT_INO)
		return ERR_PTR(-ENOSPC);

	nr = fatx_add_cluster(inode);
	if (nr < 0)
		return ERR_PTR(nr);

	sector = fatx_clus_to_blknr(FATX_SB(sb), nr);
	last_sector = sector + sec_per_clus;
	for ( ; sector < last_sector; sector++) {
		if ((bh = sb_getblk(sb, sector))) {
			memset(bh->b_data, 0xFF, sb->s_blocksize);
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
			if (!res)
				res = bh;
			else
				brelse(bh);
		}
	}
	if (res == NULL)
		res = ERR_PTR(-EIO);
	if (inode->i_size & (sb->s_blocksize - 1)) {
		fatx_fs_panic(sb, "Odd directory size");
		inode->i_size = (inode->i_size + sb->s_blocksize)
			& ~((loff_t)sb->s_blocksize - 1);
	}
	inode->i_size += FATX_SB(sb)->cluster_size;
	FATX_I(inode)->mmu_private += FATX_SB(sb)->cluster_size;

	return res;
}

int fatx_add_entries(struct inode *dir, struct buffer_head **bh,
		  struct fatx_dir_entry **de, loff_t *i_pos)
{
	loff_t offset, curr;
	struct buffer_head *new_bh;
	
	PRINTK("FATX: %s \n", __FUNCTION__);
	offset = curr = 0;
	*bh = NULL;
	
	while (fatx_get_entry(dir, &curr, bh, de, i_pos) > -1) {
		if (FATX_IS_FREE(*de)) {
			PRINTK("FATX: fatx_add_entries: found free entry\n");
			return offset;
		} 
		if (FATX_END_OF_DIR(*de)) {
			struct buffer_head *eod_bh = NULL;
			struct fatx_dir_entry *eod_de = NULL;
			loff_t eod_ino;
			PRINTK("FATX: fatx_add_entries: found EOD at %lX\n",(long)(*de));
			//make sure the next one isn't first in new cluster
			if (fatx_get_entry(dir,&curr,&eod_bh,&eod_de,&eod_ino) > -1) {
				//EOD in same cluster...find proper de and mark new EOD
				eod_de->name_length = 0xFF;
				mark_buffer_dirty(eod_bh);
				PRINTK("FATX: fatx_add_entries: marked new EOD at %lX\n",(long)eod_de);
				if(eod_bh) brelse(eod_bh);
			} else {
				//we will take the easy out...do nothing...
				//assume fat table used to indicate EOD
				//if this is wrong, need to fatx_extend_dir
				//making first entry in next cluster EOD
				printk("FATX: fatx_add_entries: EOD marked by FAT\n");
				printk("FATX: ...:offset=%08lX, curr=%08lX\n",
						(unsigned long)offset,(unsigned long)curr);
			}
			PRINTK("FATX: fatx_add_entries: using entry at %lX\n",(long)(*de));
			return offset;
		}
		offset = curr;
	}
	PRINTK("FATX: fatx_add_entries: need to extend dir\n");
	if (dir->i_ino == FATX_ROOT_INO) {
		printk("FATX: fatx_add_entries: but it's root dir...can't extend\n");
		return -ENOSPC;
	}
	new_bh = fatx_extend_dir(dir);
	if (IS_ERR(new_bh)) {
		PRINTK("FATX: fatx_add_entries: fatx_extend_dir failed...no space?\n");
		return PTR_ERR(new_bh);
	}
	brelse(new_bh);
	fatx_get_entry(dir, &curr, bh, de, i_pos);
	(*de)[1].name_length = 0xFF;
	return offset;
}

EXPORT_SYMBOL(fatx_add_entries);

int fatx_new_dir(struct inode *dir, struct inode *parent)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;

	PRINTK("FATX: %s \n", __FUNCTION__);
	bh = fatx_extend_dir(dir);
	if (IS_ERR(bh)) {
		printk("FATX: fatx_new_dir: failed to get new cluster...no space?\n");
		return PTR_ERR(bh);
	}

	/* zeroed out, so... */
	de = (struct fatx_dir_entry*)&bh->b_data[0];
	de[0].attr = de[1].attr = ATTR_DIR;
	de[0].name_length = 0xFF; //end of dir marker
	de[0].start = cpu_to_le32(FATX_I(dir)->i_logstart);
	de[1].start = cpu_to_le32(FATX_I(parent)->i_logstart);
	mark_buffer_dirty(bh);
	brelse(bh);
	dir->i_atime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(dir);

	return 0;
}

EXPORT_SYMBOL(fatx_new_dir);
