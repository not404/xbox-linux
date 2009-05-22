/*
 *  linux/fs/fatx/namei.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  Hidden files 1995 by Albert Cahalan <albert@ccs.neu.edu> <adc@coe.neu.edu>
 *  Rewritten for constant inumbers 1999 by Al Viro
 *
 *  FATX port 2005 by Edgar Hucek
 *
 */

#include <linux/module.h>
#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/fatx_fs.h>
#include <linux/smp_lock.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

/* Characters that are undesirable in an MS-DOS file name */
static unsigned char bad_chars[] = "*?<>|\"";

void fatx_printname(const char *name, int length)
{
	int i;
	for(i=0;i<length;i++) {
		printk("%c",name[i]);
	}
	printk("  len %d",length);
}

/***** Formats an MS-DOS file name. Rejects invalid names. */
int fatx_format_name(const unsigned char *name, int len,
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
		      struct fatx_slot_info *sinfo)
{
	unsigned char fatx_name[FATX_NAME];
	int err;

	err = fatx_format_name(name, len, fatx_name);
	if (err)
		return -ENOENT;

	err = fatx_scan(dir, fatx_name, sinfo);
	if (err)
		brelse(sinfo->bh);
	return err;
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
	struct fatx_slot_info sinfo;
	struct inode *inode = NULL;
	int res;

	dentry->d_op = &fatx_dentry_operations;

	lock_kernel();
	res = fatx_find(dir, dentry->d_name.name, dentry->d_name.len, &sinfo);
	if (res == -ENOENT)
		goto add;
	if (res < 0)
		goto out;
	inode = fatx_build_inode(sb, sinfo.de, sinfo.i_pos);
	brelse(sinfo.bh);
	if (IS_ERR(inode)) {
		res = PTR_ERR(inode);
		goto out;
	}
add:
	res = 0;
	dentry = d_splice_alias(inode, dentry);
	if (dentry)
		dentry->d_op = &fatx_dentry_operations;
out:
	unlock_kernel();
	if (!res)
		return dentry;
	return ERR_PTR(res);
}

/***** Creates a directory entry (name is already formatted). */
static int fatx_add_entry(struct inode *dir, const unsigned char *name, int len,
			   int is_dir, int cluster,
			   struct timespec *ts, struct fatx_slot_info *sinfo)
{
	struct fatx_dir_entry de;
	__le16 time, date;
	int err;

	memset(de.name,0xFF,FATX_NAME);
	memcpy(de.name, name, FATX_NAME);
	de.attr = is_dir ? ATTR_DIR : ATTR_ARCH;
	de.name_length = len;
	fatx_date_unix2dos(ts->tv_sec, &time, &date);
	de.cdate = de.adate = 0;
	de.ctime = 0;
	de.time = time;
	de.date = date;
	de.start = cpu_to_le32(cluster);
	de.size = 0;

	err = fatx_add_entries(dir, &de, sinfo);
	if (err)
		return err;

	dir->i_ctime = dir->i_mtime = *ts;
	if (IS_DIRSYNC(dir))
		(void)fatx_sync_inode(dir);
	else
		mark_inode_dirty(dir);

	return 0;
}

/***** Create a file */
static int fatx_create(struct inode *dir, struct dentry *dentry, int mode,
			struct nameidata *nd)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct fatx_slot_info sinfo;
	struct timespec ts;
	unsigned char fatx_name[FATX_NAME];
	int err;

	PRINTK("FATX: %s\n", __FUNCTION__ );
	lock_kernel();

	err = fatx_format_name(dentry->d_name.name, dentry->d_name.len, fatx_name);
	if (err)
		goto out;
	/* Have to do it due to foo vs. .foo conflicts */
	if (!fatx_scan(dir, fatx_name, &sinfo)) {
		brelse(sinfo.bh);
		err = -EINVAL;
		goto out;
	}

	ts = CURRENT_TIME;
	err = fatx_add_entry(dir, fatx_name, dentry->d_name.len, 0, 0, &ts, &sinfo);
	if (err)
		goto out;
	inode = fatx_build_inode(sb, sinfo.de, sinfo.i_pos);
	brelse(sinfo.bh);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	inode->i_mtime = inode->i_atime = inode->i_ctime = ts;
	/* timestamp is already written, so mark_inode_dirty() is unneeded. */

	dentry->d_time = dentry->d_parent->d_inode->i_version;
	d_instantiate(dentry, inode);

out:
	unlock_kernel();
	return err;
}

/***** Remove a directory */
static int fatx_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct fatx_slot_info sinfo;
	int err;

	PRINTK("FATX: %s\n", __FUNCTION__);
	lock_kernel();
	/*
	 * Check whether the directory is not in use, then check
	 * whether it is empty.
	 */
	err = fatx_dir_empty(inode);
	if (err) {
		PRINTK("FATX: %s fatx_remove_entries error\n", __FUNCTION__);
		goto out;
	}
	err = fatx_find(dir, dentry->d_name.name, dentry->d_name.len, &sinfo);
	if (err) {
		PRINTK("FATX: %s fatx_find error\n", __FUNCTION__);
		goto out;
	}
	err = fatx_remove_entries(dir, &sinfo);	/* and releases bh */
	if (err) {
		PRINTK("FATX: %s fatx_find error\n", __FUNCTION__);
		goto out;
	}
	dir->i_nlink--;

	inode->i_nlink = 0;
	inode->i_ctime = CURRENT_TIME;
	fatx_detach(inode);
out:
	unlock_kernel();

	return err;
}

/***** Make a directory */
static int fatx_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_slot_info sinfo;
	struct inode *inode;
	unsigned char fatx_name[FATX_NAME];
	struct timespec ts;
	int err, cluster;

	lock_kernel();

	PRINTK("FATX: %s\n", __FUNCTION__ );
	err = fatx_format_name(dentry->d_name.name, dentry->d_name.len, fatx_name);
	if (err)
		goto out;
	/* foo vs .foo situation */
	if (!fatx_scan(dir, fatx_name, &sinfo)) {
		brelse(sinfo.bh);
		PRINTK("FATX: %s fatx_scan failed\n", __FUNCTION__ );
		err = -EINVAL;
		goto out;
	}

	ts = CURRENT_TIME;
	cluster = fatx_alloc_new_dir(dir, &ts);
	if (cluster < 0) {
		PRINTK("FATX: %s fatx_alloc_new_dir failed\n", __FUNCTION__ );
		err = cluster;
		goto out;
	}
	err = fatx_add_entry(dir, fatx_name, dentry->d_name.len, 1, cluster, &ts, &sinfo);
	if (err) {
		PRINTK("FATX: %s fatx_add_entry failed\n", __FUNCTION__ );
		goto out_free;
	}

	dir->i_nlink++;

	inode = fatx_build_inode(sb, sinfo.de, sinfo.i_pos);
	brelse(sinfo.bh);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		/* the directory was completed, just return a error */
		PRINTK("FATX: %s fatx_build_inode failed\n", __FUNCTION__ );
		goto out;
	}
	inode->i_nlink = 2;
	inode->i_mtime = inode->i_atime = inode->i_ctime = ts;
	/* timestamp is already written, so mark_inode_dirty() is unneeded. */

	dentry->d_time = dentry->d_parent->d_inode->i_version;
	d_instantiate(dentry, inode);

	PRINTK("FATX: %s exit\n", __FUNCTION__ );
	unlock_kernel();
	return 0;

out_free:
	fatx_free_clusters(dir, cluster);
out:
	unlock_kernel();
	return err;
}

/***** Unlink a file */
static int fatx_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct fatx_slot_info sinfo;
	int err;

	PRINTK("FATX: %s\n", __FUNCTION__);
	lock_kernel();
	err = fatx_find(dir, dentry->d_name.name, dentry->d_name.len, &sinfo);
	if (err)
		goto out;

	err = fatx_remove_entries(dir, &sinfo);	/* and releases bh */
	if (err)
		goto out;
	inode->i_nlink = 0;
	inode->i_ctime = CURRENT_TIME;
	fatx_detach(inode);
out:
	unlock_kernel();

	return err;
}

static int do_fatx_rename(struct inode *old_dir, unsigned char *old_name,
			   struct dentry *old_dentry,
			   struct inode *new_dir, unsigned char *new_name,
			   struct dentry *new_dentry)
{
	/*
	struct buffer_head *dotdot_bh;
	struct fatx_dir_entry *dotdot_de;
	loff_t dotdot_i_pos;
	*/
	struct inode *old_inode, *new_inode;
	struct fatx_slot_info old_sinfo, sinfo;
	struct timespec ts;
	//int update_dotdot;
	int err, old_attrs, is_dir, corrupt = 0;
	int new_name_len = new_dentry->d_name.len;

	PRINTK("FATX: %s\n", __FUNCTION__ );

	//old_sinfo.bh = sinfo.bh = dotdot_bh = NULL;
	old_sinfo.bh = sinfo.bh = NULL;
	old_inode = old_dentry->d_inode;
	new_inode = new_dentry->d_inode;

	err = fatx_scan(old_dir, old_name, &old_sinfo);
	if (err) {
		err = -EIO;
		goto out;
	}

	is_dir = S_ISDIR(old_inode->i_mode);
	/*
	update_dotdot = (is_dir && old_dir != new_dir);
	if (update_dotdot) {
		if (fatx_get_dotdot_entry(old_inode, &dotdot_bh, &dotdot_de,
					 &dotdot_i_pos) < 0) {
			err = -EIO;
			goto out;
		}
	}
	*/
	old_attrs = FATX_I(old_inode)->i_attrs;
	err = fatx_scan(new_dir, new_name, &sinfo);
	if (!err) {
		if (!new_inode) {
			/* "foo" -> ".foo" case. just change the ATTR_HIDDEN */
			if (sinfo.de != old_sinfo.de) {
				err = -EINVAL;
				goto out;
			}
			FATX_I(old_inode)->i_attrs &= ~ATTR_HIDDEN;
			if (IS_DIRSYNC(old_dir)) {
				err = fatx_sync_inode(old_inode);
				if (err) {
					FATX_I(old_inode)->i_attrs = old_attrs;
					goto out;
				}
			} else
				mark_inode_dirty(old_inode);

			old_dir->i_version++;
			old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
			if (IS_DIRSYNC(old_dir))
				(void)fatx_sync_inode(old_dir);
			else
				mark_inode_dirty(old_dir);
			goto out;
		}
	}

	ts = CURRENT_TIME;
	if (new_inode) {
		if (err)
			goto out;
		if (FATX_I(new_inode)->i_pos != sinfo.i_pos) {
			/* WTF??? Cry and fail. */
			printk(KERN_WARNING "fatx_rename: fs corrupted\n");
			goto out;
		}

		if (is_dir) {
			err = fatx_dir_empty(new_inode);
			if (err)
				goto out;
		}
		fatx_detach(new_inode);
	} else {
		err = fatx_add_entry(new_dir, new_name, new_name_len, is_dir, 0,
				      &ts, &sinfo);
		if (err)
			goto out;
	}
	new_dir->i_version++;

	fatx_detach(old_inode);
	fatx_attach(old_inode, sinfo.i_pos);
	FATX_I(old_inode)->i_attrs &= ~ATTR_HIDDEN;
	if (IS_DIRSYNC(new_dir)) {
		err = fatx_sync_inode(old_inode);
		if (err)
			goto error_inode;
	} else
		mark_inode_dirty(old_inode);

	/*
	if (update_dotdot) {
		int start = FATX_I(new_dir)->i_logstart;
		dotdot_de->start = cpu_to_le32(start);
		mark_buffer_dirty(dotdot_bh);
		if (IS_DIRSYNC(new_dir)) {
			err = sync_dirty_buffer(dotdot_bh);
			if (err)
				goto error_dotdot;
		}
		old_dir->i_nlink--;
		if (!new_inode)
			new_dir->i_nlink++;
	}
	*/
	
	err = fatx_remove_entries(old_dir, &old_sinfo);	/* and releases bh */
	old_sinfo.bh = NULL;
	if (err)
		goto error_dotdot;
	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = ts;
	if (IS_DIRSYNC(old_dir))
		(void)fatx_sync_inode(old_dir);
	else
		mark_inode_dirty(old_dir);

	if (new_inode) {
		if (is_dir)
			new_inode->i_nlink -= 2;
		else
			new_inode->i_nlink--;
		new_inode->i_ctime = ts;
	}
out:
	brelse(sinfo.bh);
	brelse(old_sinfo.bh);
	return err;

error_dotdot:
	corrupt = 1;
	
	/*
	if (update_dotdot) {
		int start = FATX_I(old_dir)->i_logstart;
		dotdot_de->start = cpu_to_le32(start);
		mark_buffer_dirty(dotdot_bh);
		corrupt |= sync_dirty_buffer(dotdot_bh);
	}
	*/
error_inode:
	fatx_detach(old_inode);
	fatx_attach(old_inode, old_sinfo.i_pos);
	FATX_I(old_inode)->i_attrs = old_attrs;
	if (new_inode) {
		fatx_attach(new_inode, sinfo.i_pos);
		if (corrupt)
			corrupt |= fatx_sync_inode(new_inode);
	} else {
		/*
		 * If new entry was not sharing the data cluster, it
		 * shouldn't be serious corruption.
		 */
		int err2 = fatx_remove_entries(new_dir, &sinfo);
		if (corrupt)
			corrupt |= err2;
		sinfo.bh = NULL;
	}
	if (corrupt < 0) {
		fatx_fs_panic(new_dir->i_sb,
			     "%s: Filesystem corrupted (i_pos %lld)",
			     __FUNCTION__, sinfo.i_pos);
	}
	goto out;
}

/***** Rename, a wrapper for rename_same_dir & rename_diff_dir */
static int fatx_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	unsigned char old_fatx_name[FATX_NAME], new_fatx_name[FATX_NAME];
	int err;

	lock_kernel();

	err = fatx_format_name(old_dentry->d_name.name,
				old_dentry->d_name.len, old_fatx_name);
	if (err)
		goto out;
	err = fatx_format_name(new_dentry->d_name.name,
				new_dentry->d_name.len, new_fatx_name);
	if (err)
		goto out;

	err = do_fatx_rename(old_dir, old_fatx_name, old_dentry,
			      new_dir, new_fatx_name, new_dentry);
out:
	unlock_kernel();
	return err;
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
	return get_sb_bdev(fs_type, flags, dev_name, data, fatx_fill_super);
}

