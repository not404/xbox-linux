/*
 *  linux/fs/fatx/namei.c
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#define __NO_VERSION__
#include <linux/module.h>

#include <linux/sched.h>
#include <linux/fatx_fs.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <asm/uaccess.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

/* Characters that are undesirable in an MS-DOS file name */
  
static char bad_chars[] = "*?<>|\";";

/*
 * Formats a FATX file name. Rejects invalid names. 
 */

static int fatx_format_name( const char *name,int len,char *out_name )
{
	int i;
	char trash[FATX_MAX_NAME_LENGTH];

	if (len > FATX_MAX_NAME_LENGTH) return -EINVAL;
	
	if (out_name == NULL) out_name = trash;
	
	memset(out_name,0xFF,FATX_MAX_NAME_LENGTH);
	
	//check for bad characters in name
	for(i=0; i<len; i++) {
		if (strchr(bad_chars,name[i])) return -EINVAL;
		out_name[i] = name[i];
	}

	return 0;
}

/*
 * Locates a directory entry.  Uses unformatted name. 
 */

static int fatx_find( struct inode *dir, const char *name, int len, struct buffer_head **bh, 
		struct fatx_dir_entry **de, loff_t *i_pos)
{
	//verify its a valid name
	if (fatx_format_name(name,len,NULL) < 0) return -ENOENT;

	PRINTK("FATX: fatx_find\n");
	
	//find the name in the directory
	return fatx_scan(dir,name,len,bh,de,i_pos);

}

/* 
 * Get inode using directory and name 
 */

struct dentry *fatx_lookup(struct inode *dir,struct dentry *dentry,struct nameidata *nd)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = NULL;
	struct fatx_dir_entry *de;
	struct buffer_head *bh = NULL;
	loff_t i_pos;
	int res;
	
	PRINTK("FATX: fatx_lookup\n");

	res = fatx_find(dir, dentry->d_name.name, dentry->d_name.len, &bh,
			&de, &i_pos);

	if (res == -ENOENT)
		goto add;
	if (res < 0)
		goto out;
	inode = fatx_build_inode(sb, de, i_pos, &res);
	if (res)
		goto out;
add:
	d_add(dentry, inode);
	res = 0;
out:
	if (bh)
		brelse(bh);
	return ERR_PTR(res);
}

/* 
 * Creates a directory entry (name is already formatted). 
 */

static int fatx_add_entry(
		struct inode *dir, 
		const char *name,
		int len,
		struct buffer_head **bh,
		struct fatx_dir_entry **de,
		loff_t *i_pos,
		int is_dir )
{
	int res;

	if ((res = fatx_do_add_entry(dir, bh, de, i_pos))<0)
		return res;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(dir);
	memset((*de)->name,0xFF,FATX_MAX_NAME_LENGTH);
	memcpy((*de)->name,name,len);
	(*de)->name_length = len;
	(*de)->attr = is_dir ? ATTR_DIR : ATTR_ARCH;
	(*de)->start = 0;
	fatx_date_unix2dos(dir->i_mtime.tv_sec,&(*de)->time,&(*de)->date);
	(*de)->size = 0;
	mark_buffer_dirty(*bh);
	return 0;
}

/* 
 * Create a file 
 */

int fatx_create(struct inode *dir,struct dentry *dentry,int mode, struct nameidata *nd)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	struct inode *inode;
	loff_t i_pos;
	int res;
	const char *name = dentry->d_name.name;
	int name_length = dentry->d_name.len;
	char szFormatName[FATX_MAX_NAME_LENGTH];
	
	res = fatx_format_name(name,name_length,szFormatName);
	if (res < 0)
		return res;
	
	if (fatx_scan(dir,name,name_length,&bh,&de,&i_pos) >= 0) {
		if(bh) brelse(bh);
		return -EINVAL;
 	}
	inode = NULL;
	res = fatx_add_entry(dir, szFormatName, name_length, &bh, &de, &i_pos, 0);
	if (res)
		return res;
	inode = fatx_build_inode(dir->i_sb, de, i_pos, &res);
	if(bh) brelse(bh);
	if (!inode)
		return res;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	return 0;
}

/*
 * Remove a directory 
 */

int fatx_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	loff_t i_pos;	
	int res;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;

	bh = NULL;
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
	if(bh) brelse(bh);
	return res;
}

/*
 * Make a directory 
 */

int fatx_mkdir(struct inode *dir,struct dentry *dentry,int mode)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	struct inode *inode;
	int res;
	const char *name = dentry->d_name.name;
	int name_length = dentry->d_name.len;
	loff_t i_pos;
	char szFormatName[FATX_MAX_NAME_LENGTH];
	
	res = fatx_format_name(name,name_length,szFormatName);
	if (res < 0)
		return res;
	if (fatx_scan(dir,name,name_length,&bh,&de,&i_pos) >= 0)
		goto out_exist;

	res = fatx_add_entry(dir, szFormatName, name_length, &bh, &de, &i_pos, 1);
	if (res)
		goto out_unlock;
	inode = fatx_build_inode(dir->i_sb, de, i_pos, &res);
	if (!inode) {
		if(bh) brelse(bh);
		goto out_unlock;
	}
	res = 0;

	dir->i_nlink++;
	inode->i_nlink = 2; /* no need to mark them dirty */

	res = fatx_new_dir(inode, dir);
	if (res)
		goto mkdir_error;

	if(bh) brelse(bh);
	d_instantiate(dentry, inode);
	res = 0;

out_unlock:
	return res;

mkdir_error:
	printk(KERN_WARNING "fatx_mkdir: error=%d, attempting cleanup\n", res);
	inode->i_nlink = 0;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_nlink--;
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	de->name_length = DELETED_FLAG;
	mark_buffer_dirty(bh);
	if(bh) brelse(bh);
	fatx_detach(inode);
	iput(inode);
	goto out_unlock;

out_exist:
	if(bh) brelse(bh);
	res = -EINVAL;
	goto out_unlock;
}

/*
 * Unlink a file 
 */

int fatx_unlink( struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	loff_t i_pos;
	int res;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;

	bh = NULL;
	res = fatx_find(dir, dentry->d_name.name, dentry->d_name.len,
			&bh, &de, &i_pos);
	if (res < 0)
		goto unlink_done;

	de->name_length = DELETED_FLAG;
	mark_buffer_dirty(bh);
	fatx_detach(inode);
	if(bh) brelse(bh);
	inode->i_nlink = 0;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	res = 0;
unlink_done:
	return res;
}

static int do_fatx_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry,
		struct buffer_head *old_bh, struct fatx_dir_entry *old_de,
		int old_ino )
{
	struct buffer_head *new_bh=NULL,*dotdot_bh=NULL;
	struct fatx_dir_entry *new_de,*dotdot_de;
	struct inode *old_inode,*new_inode;
	loff_t new_ino;
	int error;
	int is_dir;
	const char *new_name = new_dentry->d_name.name;
	int new_name_len = new_dentry->d_name.len;

	PRINTK("FATX: do_fatx_rename: entered\n");
	
	old_inode = old_dentry->d_inode;
	new_inode = new_dentry->d_inode;
	is_dir = S_ISDIR(old_inode->i_mode);

	error = fatx_scan(new_dir,new_name,new_name_len,&new_bh,&new_de,&new_ino);
	if (error>=0 &&!new_inode)
		goto degenerate_case;
	if (!new_bh) {
		error = fatx_add_entry(	new_dir, new_name, new_name_len, &new_bh, 
				&new_de, &new_ino, is_dir );
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
	fatx_attach(old_inode, new_ino);
	FATX_I(old_inode)->i_attrs &= ~ATTR_HIDDEN;
	mark_inode_dirty(old_inode);
	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = old_dir->i_atime = CURRENT_TIME;
	mark_inode_dirty(old_dir);
	if (new_inode) {
		new_inode->i_nlink--;
		new_inode->i_ctime = new_inode->i_atime = CURRENT_TIME;
		mark_inode_dirty(new_inode);
	}
	if (dotdot_bh) {
		dotdot_de->start = CT_LE_L(FATX_I(new_dir)->i_logstart);
		
		PRINTK("FATX: do_fatx_rename: start = %08lX (LE=%08lX)\n",
				(long)FATX_I(new_dir)->i_logstart,
				(long)dotdot_de->start);
		
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
	if(new_bh) brelse(new_bh);
	if(dotdot_bh) brelse(dotdot_bh);
	PRINTK("FATX: do_fatx_rename: leaving (normal)\n");
	return error;

degenerate_case:
	error = -EINVAL;
	if (new_de!=old_de)
		goto out;
	FATX_I(old_inode)->i_attrs &= ~ATTR_HIDDEN;
	mark_inode_dirty(old_inode);
	old_dir->i_version++;
	old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(old_dir);
	PRINTK("FATX: do_fatx_rename: leaving (degenerate)\n");
	return 0;
}

/*
 * Rename, a wrapper for rename_same_dir & rename_diff_dir 
 */

int fatx_rename(struct inode *old_dir,struct dentry *old_dentry,
		 struct inode *new_dir,struct dentry *new_dentry)
{
	struct buffer_head *old_bh;
	struct fatx_dir_entry *old_de;
	loff_t old_ino;
	int error;

	error = fatx_format_name(old_dentry->d_name.name,old_dentry->d_name.len,NULL);
	if (error < 0)
		goto rename_done;
	error = fatx_format_name(new_dentry->d_name.name,new_dentry->d_name.len,NULL);
	if (error < 0)
		goto rename_done;

	error = fatx_scan(old_dir, old_dentry->d_name.name, old_dentry->d_name.len,
			&old_bh, &old_de, &old_ino );
	if (error < 0)
		goto rename_done;

	error = do_fatx_rename( old_dir, old_dentry, new_dir, new_dentry,
			old_bh, old_de, (ino_t)old_ino );
	if(old_bh) brelse(old_bh);

rename_done:
	return error;
}
