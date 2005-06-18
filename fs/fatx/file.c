/*
 *  linux/fs/fatx/file.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  FATX port 2005 by Edgar Hucek
 *
 *  regular file handling primitives for fatx-based filesystems
 */

#include <linux/module.h>
#include <linux/time.h>
#include <linux/fatx_fs.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

static ssize_t fatx_file_aio_write(struct kiocb *iocb, const char __user *buf,
				  size_t count, loff_t pos)
{
	struct inode *inode = iocb->ki_filp->f_dentry->d_inode;
	int retval;

	retval = generic_file_aio_write(iocb, buf, count, pos);
	if (retval > 0) {
		inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		FATX_I(inode)->i_attrs |= ATTR_ARCH;
		mark_inode_dirty(inode);
	}
	return retval;
}

static ssize_t fatx_file_writev(struct file *filp, const struct iovec *iov,
			       unsigned long nr_segs, loff_t *ppos)
{
	struct inode *inode = filp->f_dentry->d_inode;
	int retval;

	retval = generic_file_writev(filp, iov, nr_segs, ppos);
	if (retval > 0) {
		inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		FATX_I(inode)->i_attrs |= ATTR_ARCH;
		mark_inode_dirty(inode);
	}
	return retval;
}

struct file_operations fatx_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.readv		= generic_file_readv,
	.writev		= fatx_file_writev,
	.aio_read	= generic_file_aio_read,
	.aio_write	= fatx_file_aio_write,
	.mmap		= generic_file_mmap,
	.ioctl		= NULL,
	.fsync		= file_fsync,
	.sendfile	= generic_file_sendfile,
};

int fatx_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct fatx_sb_info *sbi = FATX_SB(dentry->d_sb);
	struct inode *inode = dentry->d_inode;
	int mask, error = 0;

	lock_kernel();

	/* FAT cannot truncate to a longer file */
	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size > inode->i_size) {
			error = -EPERM;
			goto out;
		}
	}

	error = inode_change_ok(inode, attr);
	if (error) {
		if (sbi->options.quiet)
			error = 0;
		goto out;
	}
	if (((attr->ia_valid & ATTR_UID) &&
	     (attr->ia_uid != sbi->options.fs_uid)) ||
	    ((attr->ia_valid & ATTR_GID) &&
	     (attr->ia_gid != sbi->options.fs_gid)) ||
	    ((attr->ia_valid & ATTR_MODE) &&
	     (attr->ia_mode & ~FATX_VALID_MODE)))
		error = -EPERM;

	if (error) {
		if (sbi->options.quiet)
			error = 0;
		goto out;
	}
	error = inode_setattr(inode, attr);
	if (error)
		goto out;

	if (S_ISDIR(inode->i_mode))
		mask = sbi->options.fs_dmask;
	else
		mask = sbi->options.fs_fmask;
	inode->i_mode &= S_IFMT | (S_IRWXUGO & ~mask);
out:
	unlock_kernel();
	return error;
}

EXPORT_SYMBOL(fatx_notify_change);

/* Free all clusters after the skip'th cluster. */
static int fatx_free(struct inode *inode, int skip)
{
	struct super_block *sb = inode->i_sb;
	int err, wait, free_start, i_start, i_logstart;

	if (FATX_I(inode)->i_start == 0)
		return 0;

	/*
	 * Write a new EOF, and get the remaining cluster chain for freeing.
	 */
	wait = IS_DIRSYNC(inode);
	if (skip) {
		struct fatx_entry fatxent;
		__s64 ret;
		int fclus, dclus;

		ret = fatx_get_cluster(inode, skip - 1, &fclus, &dclus);
		if (ret < 0)
			return ret;
		else if (ret == FAT_ENT_EOF)
			return 0;

		fatxent_init(&fatxent);
		ret = fatx_ent_read(inode, &fatxent, dclus);
		if (ret == FAT_ENT_EOF) {
			fatxent_brelse(&fatxent);
			return 0;
		} else if (ret == FAT_ENT_FREE) {
			fatx_fs_panic(sb,
				     "%s: invalid cluster chain (i_pos %lld)",
				     __FUNCTION__, FATX_I(inode)->i_pos);
			ret = -EIO;
		} else if (ret > 0) {
			err = fatx_ent_write(inode, &fatxent, FAT_ENT_EOF, wait);
			if (err)
				ret = err;
		}
		fatxent_brelse(&fatxent);
		if (ret < 0)
			return ret;

		free_start = ret;
		i_start = i_logstart = 0;
		fatx_cache_inval_inode(inode);
	} else {
		fatx_cache_inval_inode(inode);

		i_start = free_start = FATX_I(inode)->i_start;
		i_logstart = FATX_I(inode)->i_logstart;
		FATX_I(inode)->i_start = 0;
		FATX_I(inode)->i_logstart = 0;
	}
	FATX_I(inode)->i_attrs |= ATTR_ARCH;
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	if (wait) {
		err = fatx_sync_inode(inode);
		if (err)
			goto error;
	} else
		mark_inode_dirty(inode);
	inode->i_blocks = skip << (FATX_SB(sb)->cluster_bits - 9);

	/* Freeing the remained cluster chain */
	return fatx_free_clusters(inode, free_start);

error:
	if (i_start) {
		FATX_I(inode)->i_start = i_start;
		FATX_I(inode)->i_logstart = i_logstart;
	}
	return err;
}

void fatx_truncate(struct inode *inode)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	const unsigned int cluster_size = sbi->cluster_size;
	int nr_clusters;

	/*
	 * This protects against truncating a file bigger than it was then
	 * trying to write into the hole.
	 */
	if (FATX_I(inode)->mmu_private > inode->i_size)
		FATX_I(inode)->mmu_private = inode->i_size;

	nr_clusters = (inode->i_size + (cluster_size - 1)) >> sbi->cluster_bits;

	lock_kernel();
	fatx_free(inode, nr_clusters);
	unlock_kernel();
}

struct inode_operations fatx_file_inode_operations = {
	.truncate	= fatx_truncate,
	.setattr	= fatx_notify_change,
};
