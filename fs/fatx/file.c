/*
 *  linux/fs/fatx/file.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  FATX port 2005 by Edgar Hucek
 *
 *  regular file handling primitives for fatx-based filesystems
 */

#include <linux/module.h>
#include <linux/time.h>
#include <linux/fatx_fs.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>

#define PRINTK(format, args...) do { if (debug) printk( format, ##args ); } while(0)

static ssize_t fatx_file_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *ppos)
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

struct file_operations fatx_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_file_read,
	.write		= fatx_file_write,
	.mmap		= generic_file_mmap,
	.fsync		= file_fsync,
	.readv		= generic_file_readv,
	.writev		= generic_file_writev,
	.sendfile	= generic_file_sendfile,
};

int fatx_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct fatx_sb_info *sbi = FATX_SB(dentry->d_sb);
	struct inode *inode = dentry->d_inode;
	int error = 0;

	PRINTK("FATX: %s \n", __FUNCTION__);
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

	inode->i_mode = ((inode->i_mode & S_IFMT) | ((((inode->i_mode & S_IRWXU
		& ~sbi->options.fs_umask) | S_IRUSR) >> 6)*S_IXUGO)) &
		~sbi->options.fs_umask;

out:
	unlock_kernel();
	return error;
}

EXPORT_SYMBOL(fatx_notify_change);

/* Free all clusters after the skip'th cluster. */
static int fatx_free(struct inode *inode, int skip)
{
	struct super_block *sb = inode->i_sb;
	__s64 nr, ret; 
	int fclus, dclus;

	PRINTK("FATX: %s \n", __FUNCTION__);
	if (FATX_I(inode)->i_start == 0)
		return 0;

	if (skip) {
		ret = fatx_get_cluster(inode, skip - 1, &fclus, &dclus);
		if (ret < 0)
			return ret;
		else if (ret == FAT_ENT_EOF)
			return 0;

		nr = fatx_access(sb, dclus, -1);
		if (nr == FAT_ENT_EOF)
			return 0;
		else if (nr > 0) {
			/*
			 * write a new EOF, and get the remaining cluster
			 * chain for freeing.
			 */
			nr = fatx_access(sb, dclus, FAT_ENT_EOF);
		}
		if (nr < 0)
			return nr;

		fatx_cache_inval_inode(inode);
	} else {
		fatx_cache_inval_inode(inode);

		nr = FATX_I(inode)->i_start;
		FATX_I(inode)->i_start = 0;
		FATX_I(inode)->i_logstart = 0;
		mark_inode_dirty(inode);
	}

	lock_fatx(sb);
	do {
		nr = fatx_access(sb, nr, FAT_ENT_FREE);
		if (nr < 0)
			goto error;
		else if (nr == FAT_ENT_FREE) {
			fatx_fs_panic(sb, "%s: deleting beyond EOF (i_pos %lld)",
				     __FUNCTION__, FATX_I(inode)->i_pos);
			nr = -EIO;
			goto error;
		}
		if (FATX_SB(sb)->free_clusters != -1)
			FATX_SB(sb)->free_clusters++;
		inode->i_blocks -= FATX_SB(sb)->cluster_size >> 9;
	} while (nr != FAT_ENT_EOF);
	nr = 0;
error:
	unlock_fatx(sb);

	return nr;
}

void fatx_truncate(struct inode *inode)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	const unsigned int cluster_size = sbi->cluster_size;
	int nr_clusters;

	PRINTK("FATX: %s \n", __FUNCTION__);
	/*
	 * This protects against truncating a file bigger than it was then
	 * trying to write into the hole.
	 */
	if (FATX_I(inode)->mmu_private > inode->i_size)
		FATX_I(inode)->mmu_private = inode->i_size;

	nr_clusters = (inode->i_size + (cluster_size - 1)) >> sbi->cluster_bits;

	lock_kernel();
	fatx_free(inode, nr_clusters);
	FATX_I(inode)->i_attrs |= ATTR_ARCH;
	unlock_kernel();
	inode->i_ctime = inode->i_mtime = inode->i_atime = CURRENT_TIME;
	mark_inode_dirty(inode);
}

struct inode_operations fatx_file_inode_operations = {
	.truncate	= fatx_truncate,
	.setattr	= fatx_notify_change,
};
