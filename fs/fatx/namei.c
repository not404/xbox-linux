/*
 *  linux/fs/fatx/namei.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  FATX port 2005 by Edgar Hucek
 *  Hidden files 1995 by Albert Cahalan <albert@ccs.neu.edu> <adc@coe.neu.edu>
 *  Rewritten for constant inumbers 1999 by Al Viro
 */

#include <linux/module.h>
#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/fatx_fs.h>
#include <linux/smp_lock.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

/* Characters that are undesirable in an MS-DOS file name */
static unsigned char bad_chars[] = "*?<>|\"";

/***** Formats an MS-DOS file name. Rejects invalid names. */
static int fatx_format_name(const unsigned char *name, int len,
			     unsigned char *out_name)
{
	int i;
	char trash[FATX_NAME];

	if (len > FATX_NAME) return -EINVAL;

	if (out_name == NULL) out_name = trash;

	memset(out_name,0xFF,FATX_NAME);

	//check for bad characters in name
	for(i=0; i<len; i++) {
		if (strchr(bad_chars,name[i])) return -EINVAL;
			out_name[i] = name[i];
	}
	
	return 0;
}

/***** Locates a directory entry.  Uses unformatted name. */
static int fatx_find(struct inode *dir, const unsigned char *name, int len,
		      struct buffer_head **bh, struct fatx_dir_entry **de,
		      loff_t *i_pos)
{
	unsigned char fatx_name[FATX_NAME];
	int res;

	PRINTK("FATX: %s \n", __FUNCTION__);
	res = fatx_format_name(name, len, fatx_name);
	if (res < 0)
		return -ENOENT;
	res = fatx_scan(dir, fatx_name, bh, de, i_pos);

	return res;
}

/*
 * Compute the hash for the fatx name corresponding to the dentry.
 * Note: if the name is invalid, we leave the hash code unchanged so
 * that the existing dentry can be used. The fatx fs routines will
 * return ENOENT or EINVAL as appropriate.
 */
static int fatx_hash(struct dentry *dentry, struct qstr *qstr)
{
	unsigned char fatx_name[FATX_NAME];
	int error;

	error = fatx_format_name(qstr->name, qstr->len, fatx_name);
	if (!error)
		qstr->hash = full_name_hash(fatx_name, FATX_NAME);
	return 0;
}

/*
 * Compare two fatx names. If either of the names are invalid,
 * we fall back to doing the standard name comparison.
 */
static int fatx_cmp(struct dentry *dentry, struct qstr *a, struct qstr *b)
{
	unsigned char a_fatx_name[FATX_NAME], b_fatx_name[FATX_NAME];
	int error;

	error = fatx_format_name(a->name, a->len, a_fatx_name);
	if (error)
		goto old_compare;
	error = fatx_format_name(b->name, b->len, b_fatx_name);
	if (error)
		goto old_compare;
	error = memcmp(a_fatx_name, b_fatx_name, FATX_NAME);
out:
	return error;

old_compare:
	error = 1;
	if (a->len == b->len)
		error = memcmp(a->name, b->name, a->len);
	goto out;
}

static struct dentry_operations fatx_dentry_operations = {
	.d_hash		= fatx_hash,
	.d_compare	= fatx_cmp,
};

/*
 * AV. Wrappers for FAT sb operations. Is it wise?
 */

/***** Get inode using directory and name */
static struct dentry *fatx_lookup(struct inode *dir, struct dentry *dentry,
				   struct nameidata *nd)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = NULL;
	struct fatx_dir_entry *de;
	struct buffer_head *bh = NULL;
	loff_t i_pos;
	int res;

	PRINTK("FATX: %s \n", __FUNCTION__);
	dentry->d_op = &fatx_dentry_operations;

	lock_kernel();
	res = fatx_find(dir, dentry->d_name.name, dentry->d_name.len, &bh,
			 &de, &i_pos);
	if (res == -ENOENT)
		goto add;
	if (res < 0)
		goto out;
	inode = fatx_build_inode(sb, de, i_pos, &res);
//	PRINTK("FATX: fatx_lookup i_pos %lld res %d de->start %d de->name %s\n", i_pos, res, de->start, de->name);
	if (res)
		goto out;
add:
	res = 0;
	dentry = d_splice_alias(inode, dentry);
	if (dentry)
		dentry->d_op = &fatx_dentry_operations;
out:
	brelse(bh);
	unlock_kernel();
	if (!res)
		return dentry;
	return ERR_PTR(res);
}

/***** Creates a directory entry (name is already formatted). */
static int fatx_add_entry(struct inode *dir, const unsigned char *name, int len,
			   struct buffer_head **bh,
			   struct fatx_dir_entry **de,
			   loff_t *i_pos, int is_dir)
{
	int res;

	PRINTK("FATX: %s \n", __FUNCTION__);
	res = fatx_add_entries(dir, bh, de, i_pos);
	if (res < 0)
		return res;

	/*
	 * XXX all times should be set by caller upon successful completion.
	 */
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(dir);
	memset((*de)->name,0xFF,FATX_NAME);
	memcpy((*de)->name, name, len);
	(*de)->name_length = len;
	(*de)->attr = is_dir ? ATTR_DIR : ATTR_ARCH;
	(*de)->start = 0;
	fatx_date_unix2dos(dir->i_mtime.tv_sec, &(*de)->time, &(*de)->date);
	(*de)->size = 0;
	mark_buffer_dirty(*bh);
	return 0;
}

/***** Create a file */
static int fatx_create(struct inode *dir, struct dentry *dentry, int mode,
			struct nameidata *nd)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	struct inode *inode;
	loff_t i_pos;
	int res;
	unsigned char fatx_name[FATX_NAME];

	PRINTK("FATX: %s \n", __FUNCTION__);
	lock_kernel();
	res = fatx_format_name(dentry->d_name.name, dentry->d_name.len,	fatx_name);
	if (res < 0) {
		unlock_kernel();
		return res;
	}
	if (fatx_scan(dir, fatx_name, &bh, &de, &i_pos) >= 0) {
		brelse(bh);
		unlock_kernel();
		return -EINVAL;
	}
	inode = NULL;
	res = fatx_add_entry(dir, fatx_name, dentry->d_name.len, &bh, &de, &i_pos, 0);
	if (res) {
		unlock_kernel();
		return res;
	}
	inode = fatx_build_inode(dir->i_sb, de, i_pos, &res);
	brelse(bh);
	if (!inode) {
		unlock_kernel();
		return res;
	}
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	unlock_kernel();
	return 0;
}

/***** Remove a directory */
static int fatx_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	loff_t i_pos;
	int res;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;

	PRINTK("FATX: %s \n", __FUNCTION__);
	bh = NULL;
	lock_kernel();
	res = fatx_find(dir, dentry->d_name.name, dentry->d_name.len,
			 &bh, &de, &i_pos);
	if (res < 0)
		goto rmdir_done;
	/*
	 * Check whether the directory is not in use, then check
	 * whether it is empty.
	 */
	res = fatx_dir_empty(inode);
	if (res)
		goto rmdir_done;

	de->name_length = DELETED_FLAG;
	mark_buffer_dirty(bh);
	fatx_detach(inode);
	inode->i_nlink = 0;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_nlink--;
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	res = 0;

rmdir_done:
	brelse(bh);
	unlock_kernel();
	return res;
}

/***** Make a directory */
static int fatx_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	struct inode *inode;
	int res;
	unsigned char fatx_name[FATX_NAME];
	loff_t i_pos;

	PRINTK("FATX: %s \n", __FUNCTION__);
	lock_kernel();
	res = fatx_format_name(dentry->d_name.name, dentry->d_name.len,fatx_name);
	if (res < 0) {
		unlock_kernel();
		return res;
	}
	if (fatx_scan(dir, fatx_name, &bh, &de, &i_pos) >= 0)
		goto out_exist;

	res = fatx_add_entry(dir, fatx_name, dentry->d_name.len, &bh, &de, &i_pos, 1);
	if (res)
		goto out_unlock;
	inode = fatx_build_inode(dir->i_sb, de, i_pos, &res);
	if (!inode) {
		brelse(bh);
		goto out_unlock;
	}
	res = 0;

	dir->i_nlink++;
	inode->i_nlink = 2;	/* no need to mark them dirty */

	res = fatx_new_dir(inode, dir);
	if (res)
		goto mkdir_error;

	brelse(bh);
	d_instantiate(dentry, inode);
	res = 0;

out_unlock:
	unlock_kernel();
	return res;

mkdir_error:
	inode->i_nlink = 0;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_nlink--;
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	de->name_length = DELETED_FLAG;
	mark_buffer_dirty(bh);
	brelse(bh);
	fatx_detach(inode);
	iput(inode);
	goto out_unlock;

out_exist:
	brelse(bh);
	res = -EINVAL;
	goto out_unlock;
}

/***** Unlink a file */
static int fatx_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	loff_t i_pos;
	int res;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;

	PRINTK("FATX: %s \n", __FUNCTION__);
	bh = NULL;
	lock_kernel();
	res = fatx_find(dir, dentry->d_name.name, dentry->d_name.len,
			 &bh, &de, &i_pos);
	if (res < 0)
		goto unlink_done;

	de->name_length = DELETED_FLAG;
	mark_buffer_dirty(bh);
	fatx_detach(inode);
	brelse(bh);
	inode->i_nlink = 0;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	res = 0;
unlink_done:
	unlock_kernel();
	return res;
}

static int do_fatx_rename(struct inode *old_dir, unsigned char *old_name,
			   struct dentry *old_dentry,
			   struct inode *new_dir, unsigned char *new_name,
			   struct dentry *new_dentry,
			   struct buffer_head *old_bh,
			   struct fatx_dir_entry *old_de, loff_t old_i_pos)
{
	struct buffer_head *new_bh = NULL, *dotdot_bh = NULL;
	struct fatx_dir_entry *new_de, *dotdot_de;
	struct inode *old_inode, *new_inode;
	loff_t new_i_pos;
	int error;
	int is_dir;
	int new_name_len = new_dentry->d_name.len;

	PRINTK("FATX: %s \n", __FUNCTION__);
	old_inode = old_dentry->d_inode;
	new_inode = new_dentry->d_inode;
	is_dir = S_ISDIR(old_inode->i_mode);

	if (fatx_scan(new_dir, new_name, &new_bh, &new_de, &new_i_pos) >= 0 &&
	    !new_inode)
		goto degenerate_case;
	if (is_dir) {
		if (new_inode) {
			error = fatx_dir_empty(new_inode);
			if (error)
				goto out;
		}
	}
	if (!new_bh) {
		error = fatx_add_entry(new_dir, new_name, new_name_len, &new_bh, &new_de,
					&new_i_pos, is_dir);
		if (error)
			goto out;
	}
	new_dir->i_version++;

	/* There we go */

	if (new_inode)
		fatx_detach(new_inode);
	old_de->name_length = DELETED_FLAG;
	mark_buffer_dirty(old_bh);
	fatx_detach(old_inode);
	fatx_attach(old_inode, new_i_pos);
	FATX_I(old_inode)->i_attrs &= ~ATTR_HIDDEN;
	mark_inode_dirty(old_inode);
	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = old_dir->i_atime = CURRENT_TIME;
	mark_inode_dirty(old_dir);
	if (new_inode) {
		new_inode->i_nlink--;
		new_inode->i_ctime = CURRENT_TIME;
		mark_inode_dirty(new_inode);
	}
	if (dotdot_bh) {
		dotdot_de->start = cpu_to_le32(FATX_I(new_dir)->i_logstart);
		mark_buffer_dirty(dotdot_bh);
		old_dir->i_nlink--;
		mark_inode_dirty(old_dir);
		if (new_inode) {
			new_inode->i_nlink--;
			mark_inode_dirty(new_inode);
		} else {
			new_dir->i_nlink++;
			mark_inode_dirty(new_dir);
		}
	}
	error = 0;
out:
	brelse(new_bh);
	brelse(dotdot_bh);
	return error;

degenerate_case:
	error = -EINVAL;
	if (new_de != old_de)
		goto out;
	FATX_I(old_inode)->i_attrs &= ~ATTR_HIDDEN;
	mark_inode_dirty(old_inode);
	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(old_dir);
	return 0;
}

/***** Rename, a wrapper for rename_same_dir & rename_diff_dir */
static int fatx_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	struct buffer_head *old_bh;
	struct fatx_dir_entry *old_de;
	loff_t old_i_pos;
	int error;
	unsigned char old_fatx_name[FATX_NAME], new_fatx_name[FATX_NAME];

	PRINTK("FATX: %s \n", __FUNCTION__);
	lock_kernel();
	error = fatx_format_name(old_dentry->d_name.name,
				  old_dentry->d_name.len, old_fatx_name);
	if (error < 0)
		goto rename_done;
	error = fatx_format_name(new_dentry->d_name.name,
				  new_dentry->d_name.len, new_fatx_name);
	if (error < 0)
		goto rename_done;

	error = fatx_scan(old_dir, old_fatx_name, &old_bh, &old_de, &old_i_pos);
	if (error < 0)
		goto rename_done;

	error = do_fatx_rename(old_dir, old_fatx_name, old_dentry,
				new_dir, new_fatx_name, new_dentry,
				old_bh, old_de, old_i_pos);
	brelse(old_bh);

rename_done:
	unlock_kernel();
	return error;
}

static struct inode_operations fatx_dir_inode_operations = {
	.create		= fatx_create,
	.lookup		= fatx_lookup,
	.unlink		= fatx_unlink,
	.mkdir		= fatx_mkdir,
	.rmdir		= fatx_rmdir,
	.rename		= fatx_rename,
	.setattr	= fatx_notify_change,
};

static int fatx_fill_super(struct super_block *sb, void *data, int silent)
{
	int res;

	PRINTK("FATX: %s \n", __FUNCTION__);
	res = fatx_fill_super_inode(sb, data, silent, &fatx_dir_inode_operations);
	if (res)
		return res;

	sb->s_root->d_op = &fatx_dentry_operations;
	return 0;
}

struct super_block *fatx_get_sb(struct file_system_type *fs_type,
					int flags, const char *dev_name,
					void *data)
{
	PRINTK("FATX: %s \n", __FUNCTION__);
	return get_sb_bdev(fs_type, flags, dev_name, data, fatx_fill_super);
}

