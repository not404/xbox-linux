/*
 *  linux/fs/fatx/file.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  FATX port 2005 by Edgar Hucek
 *
 *  regular file handling primitives for fatx-based filesystems
 */

#include <linux/capability.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/time.h>
#include <linux/fatx_fs.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/fsnotify.h>
#include <linux/security.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

static ssize_t fatx_file_aio_write(struct kiocb *iocb, const struct iovec *buf, unsigned long count, loff_t pos)
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

int fatx_generic_ioctl(struct inode *inode, struct file *filp,
		      unsigned int cmd, unsigned long arg)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	u32 __user *user_attr = (u32 __user *)arg;

	switch (cmd) {
	case FATX_IOCTL_GET_ATTRIBUTES:
	{
		u32 attr;

		if (inode->i_ino == FATX_ROOT_INO)
			attr = ATTR_DIR;
		else
			attr = fatx_attr(inode);

		return put_user(attr, user_attr);
	}
	case FATX_IOCTL_SET_ATTRIBUTES:
	{
		u32 attr, oldattr;
		int err, is_dir = S_ISDIR(inode->i_mode);
		struct iattr ia;

		err = get_user(attr, user_attr);
		if (err)
			return err;

		mutex_lock(&inode->i_mutex);

		err = mnt_want_write(filp->f_path.mnt);
		if (err)
			goto up_no_drop_write;

		/*
		 * ATTR_VOLUME and ATTR_DIR cannot be changed; this also
		 * prevents the user from turning us into a VFAT
		 * longname entry.  Also, we obviously can't set
		 * any of the NTFS attributes in the high 24 bits.
		 */
		attr &= 0xff & ~(ATTR_VOLUME | ATTR_DIR);
		/* Merge in ATTR_VOLUME and ATTR_DIR */
		attr |= (FATX_I(inode)->i_attrs & ATTR_VOLUME) |
			(is_dir ? ATTR_DIR : 0);
		oldattr = fatx_attr(inode);

		/* Equivalent to a chmod() */
		ia.ia_valid = ATTR_MODE | ATTR_CTIME;
		ia.ia_ctime = current_fs_time(inode->i_sb);
		if (is_dir) {
			ia.ia_mode = FATX_MKMODE(attr,
				S_IRWXUGO & ~sbi->options.fs_dmask)
				| S_IFDIR;
		} else {
			ia.ia_mode = FATX_MKMODE(attr,
				(S_IRUGO | S_IWUGO | (inode->i_mode & S_IXUGO))
				& ~sbi->options.fs_fmask)
				| S_IFREG;
		}

		/* The root directory has no attributes */
		if (inode->i_ino == FATX_ROOT_INO && attr != ATTR_DIR) {
			err = -EINVAL;
			goto up;
		}

		if (sbi->options.sys_immutable) {
			if ((attr | oldattr) & ATTR_SYS) {
				if (!capable(CAP_LINUX_IMMUTABLE)) {
					err = -EPERM;
					goto up;
				}
			}
		}

		/*
		 * The security check is questionable...  We single
		 * out the RO attribute for checking by the security
		 * module, just because it maps to a file mode.
		 */
		err = security_inode_setattr(filp->f_path.dentry, &ia);
		if (err)
			goto up;

		/* This MUST be done before doing anything irreversible... */
		err = fatx_setattr(filp->f_path.dentry, &ia);
		if (err)
			goto up;

		fsnotify_change(filp->f_path.dentry, ia.ia_valid);

		if (sbi->options.sys_immutable) {
			if (attr & ATTR_SYS)
				inode->i_flags |= S_IMMUTABLE;
			else
				inode->i_flags &= S_IMMUTABLE;
		}

		FATX_I(inode)->i_attrs = attr & ATTR_UNUSED;
		mark_inode_dirty(inode);
up:
		mnt_drop_write(filp->f_path.mnt);
up_no_drop_write:
		mutex_unlock(&inode->i_mutex);
		return err;
	}
	default:
		return -ENOTTY;	/* Inappropriate ioctl for device */
	}
}

static int fatx_file_release(struct inode *inode, struct file *filp)
{
	if ((filp->f_mode & FMODE_WRITE) &&
		FATX_SB(inode->i_sb)->options.flush) {
		fatx_flush_inodes(inode->i_sb, inode, NULL);
		congestion_wait(WRITE, HZ/10);
	}
	return 0;
}

const struct file_operations fatx_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= generic_file_aio_read,
	.aio_write	= fatx_file_aio_write, // generic_file_aio_write,
	.mmap		= generic_file_mmap,
	.release	= fatx_file_release,
	.ioctl		= NULL, // fatx_generic_ioctl,
	.fsync		= file_fsync,
	.splice_read	= generic_file_splice_read,
};

static int fatx_cont_expand(struct inode *inode, loff_t size)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t start = inode->i_size, count = size - inode->i_size;
	int err;

	err = generic_cont_expand_simple(inode, size);
	if (err)
		goto out;

	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
	if (IS_SYNC(inode))
		err = sync_page_range_nolock(inode, mapping, start, count);
out:
	return err;
}

/* Free all clusters after the skip'th cluster. */
static int fatx_free(struct inode *inode, int skip)
{
	struct super_block *sb = inode->i_sb;
	int err, wait, free_start, i_start, i_logstart;

	if (FATX_I(inode)->i_start == 0)
		return 0;

	fatx_cache_inval_inode(inode);

	wait = IS_DIRSYNC(inode);
	i_start = free_start = FATX_I(inode)->i_start;
	i_logstart = FATX_I(inode)->i_logstart;

	/* First, we write the new file size. */
	if (!skip) {
		FATX_I(inode)->i_start = 0;
		FATX_I(inode)->i_logstart = 0;
	}
	FATX_I(inode)->i_attrs |= ATTR_ARCH;
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	if (wait) {
		err = fatx_sync_inode(inode);
		if (err) {
			FATX_I(inode)->i_start = i_start;
			FATX_I(inode)->i_logstart = i_logstart;
			return err;
		}
	} else
		mark_inode_dirty(inode);

	/* Write a new EOF, and get the remaining cluster chain for freeing. */
	if (skip) {
		struct fatx_entry fatxent;
		int ret, fclus, dclus;

		ret = fatx_get_cluster(inode, skip - 1, &fclus, &dclus);
		if (ret < 0)
			return ret;
		else if (ret == FATX_ENT_EOF)
			return 0;

		fatxent_init(&fatxent);
		ret = fatx_ent_read(inode, &fatxent, dclus);
		if (ret == FATX_ENT_EOF) {
			fatxent_brelse(&fatxent);
			return 0;
		} else if (ret == FATX_ENT_FREE) {
			fatx_fs_panic(sb,
				     "%s: invalid cluster chain (i_pos %lld)",
				     __func__, FATX_I(inode)->i_pos);
			ret = -EIO;
		} else if (ret > 0) {
			err = fatx_ent_write(inode, &fatxent, FATX_ENT_EOF, wait);
			if (err)
				ret = err;
		}
		fatxent_brelse(&fatxent);
		if (ret < 0)
			return ret;

		free_start = ret;
	}
	inode->i_blocks = skip << (FATX_SB(sb)->cluster_bits - 9);

	/* Freeing the remained cluster chain */
	return fatx_free_clusters(inode, free_start);
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

	fatx_free(inode, nr_clusters);
	fatx_flush_inodes(inode->i_sb, inode, NULL);
}

int fatx_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	generic_fillattr(inode, stat);
	stat->blksize = FATX_SB(inode->i_sb)->cluster_size;
	return 0;
}
EXPORT_SYMBOL_GPL(fatx_getattr);

static int fatx_sanitize_mode(const struct fatx_sb_info *sbi,
			     struct inode *inode, umode_t *mode_ptr)
{
	mode_t mask, perm;

	/*
	 * Note, the basic check is already done by a caller of
	 * (attr->ia_mode & ~FATX_VALID_MODE)
	 */

	if (S_ISREG(inode->i_mode))
		mask = sbi->options.fs_fmask;
	else
		mask = sbi->options.fs_dmask;

	perm = *mode_ptr & ~(S_IFMT | mask);

	/*
	 * Of the r and x bits, all (subject to umask) must be present. Of the
	 * w bits, either all (subject to umask) or none must be present.
	 */

	if ((perm & (S_IRUGO | S_IXUGO)) != (inode->i_mode & (S_IRUGO|S_IXUGO)))
		return -EPERM;
	if ((perm & S_IWUGO) && ((perm & S_IWUGO) != (S_IWUGO & ~mask)))
		return -EPERM;

	*mode_ptr &= S_IFMT | perm;

	return 0;
}

static int fatx_allow_set_time(struct fatx_sb_info *sbi, struct inode *inode)
{
	mode_t allow_utime = sbi->options.allow_utime;

	if (current->fsuid != inode->i_uid) {
		if (in_group_p(inode->i_gid))
			allow_utime >>= 3;
		if (allow_utime & MAY_WRITE)
			return 1;
	}

	/* use a default check */
	return 0;
}

#define TIMES_SET_FLAGS	(ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET)

int fatx_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct fatx_sb_info *sbi = FATX_SB(dentry->d_sb);
	struct inode *inode = dentry->d_inode;
	int error = 0;
	unsigned int ia_valid;

	/*
	 * Expand the file. Since inode_setattr() updates ->i_size
	 * before calling the ->truncate(), but FAT needs to fill the
	 * hole before it.
	 */
	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size > inode->i_size) {
			error = fatx_cont_expand(inode, attr->ia_size);
			if (error || attr->ia_valid == ATTR_SIZE)
				goto out;
			attr->ia_valid &= ~ATTR_SIZE;
		}
	}

	/* Check for setting the inode time. */
	ia_valid = attr->ia_valid;
	if (ia_valid & TIMES_SET_FLAGS) {
		if (fatx_allow_set_time(sbi, inode))
			attr->ia_valid &= ~TIMES_SET_FLAGS;
	}

	error = inode_change_ok(inode, attr);
	attr->ia_valid = ia_valid;
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

	/*
	 * We don't return -EPERM here. Yes, strange, but this is too
	 * old behavior.
	 */
	if (attr->ia_valid & ATTR_MODE) {
		if (fatx_sanitize_mode(sbi, inode, &attr->ia_mode) < 0)
			attr->ia_valid &= ~ATTR_MODE;
	}

	error = inode_setattr(inode, attr);
out:
	return error;
}

EXPORT_SYMBOL(fatx_setattr);

const struct inode_operations fatx_file_inode_operations = {
	.truncate	= fatx_truncate,
	.setattr	= fatx_setattr,
	.getattr	= fatx_getattr,
};
