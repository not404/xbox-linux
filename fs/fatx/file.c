/*
 *  linux/fs/fatx/file.c
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/fatx_fs.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/pagemap.h>

#include <asm/uaccess.h>
#include <asm/system.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

struct file_operations fatx_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= fatx_file_read,
	.write		= fatx_file_write,
	.mmap		= generic_file_mmap,
	.fsync		= file_fsync,
};

struct inode_operations fatx_file_inode_operations = {
	.truncate	= fatx_truncate,
	.setattr	= fatx_notify_change,
};

ssize_t fatx_file_read(	struct file *filp, char *buf, size_t count, loff_t *ppos)
{
	return generic_file_read(filp,buf,count,ppos);
}


int fatx_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	unsigned long phys;

	phys = fatx_bmap(inode, iblock);
	if (phys) {
		map_bh(bh_result, sb, phys);
		return 0;
	}
	if (!create)
		return 0;
	if (iblock << sb->s_blocksize_bits != FATX_I(inode)->mmu_private) {
		BUG();
		return -EIO;
	}
	if (!(iblock % FATX_SB(inode->i_sb)->cluster_size)) {
		if (fatx_add_cluster(inode) < 0)
			return -ENOSPC;
	}
	FATX_I(inode)->mmu_private += sb->s_blocksize;
	phys = fatx_bmap(inode, iblock);
	if (!phys)
		BUG();
	set_buffer_new(bh_result);
	map_bh(bh_result, sb, phys);
	return 0;
}

ssize_t fatx_file_write(struct file *filp, const char *buf, size_t count, loff_t *ppos)
{
	struct inode *inode = filp->f_dentry->d_inode;
	int retval;

	retval = generic_file_write(filp, buf, count, ppos);
	if (retval > 0) {
		inode->i_mtime = inode->i_ctime = inode->i_atime = CURRENT_TIME;
		FATX_I(inode)->i_attrs |= ATTR_ARCH;
		mark_inode_dirty(inode);
	}
	return retval;
}

void fatx_truncate(struct inode *inode)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	int cluster;

	/* Why no return value?  Surely the disk could fail... */
	if (IS_RDONLY (inode))
		return /* -EPERM */;
	if (IS_IMMUTABLE(inode))
		return /* -EPERM */;
	cluster = 1 << sbi->cluster_bits;
	/* 
	 * This protects against truncating a file bigger than it was then
	 * trying to write into the hole.
	 */
	if (FATX_I(inode)->mmu_private > inode->i_size)
		FATX_I(inode)->mmu_private = inode->i_size;

	fatx_free(inode, (inode->i_size + (cluster - 1)) >> sbi->cluster_bits);
	FATX_I(inode)->i_attrs |= ATTR_ARCH;
	inode->i_ctime = inode->i_mtime = inode->i_atime = CURRENT_TIME;
	mark_inode_dirty(inode);
}
