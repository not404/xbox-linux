/*
 *  linux/fs/fatx/dir.c
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#include <linux/fs.h>
#include <linux/fatx_fs.h>
#include <linux/nls.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/dirent.h>
#include <linux/mm.h>
#include <linux/ctype.h>

#include <asm/uaccess.h>

#define DEBUG
#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

static inline void fatx_printname(const char *name, int length)
{
	int i;
	for(i=0;i<length;i++) {
		PRINTK("%c",name[i]);
	}
}

/*
 * Now an ugly part: this set of directory scan routines works on clusters
 * rather than on inodes and sectors. They are necessary to locate the '..'
 * directory "inode". raw_scan_sector operates in four modes:
 *
 * name     number   ino      action
 * -------- -------- -------- -------------------------------------------------
 * non-NULL -        X        Find an entry with that name
 * NULL     non-NULL non-NULL Find an entry whose data starts at *number
 * NULL     non-NULL NULL     Count subdirectories in *number. (*)
 * NULL     NULL     non-NULL Find an empty entry
 *
 * (*) The return code should be ignored. It DOES NOT indicate success or
 *     failure. *number has to be initialized to zero.
 *
 * - = not used, X = a value is returned unless NULL
 *
 * If res_bh is non-NULL, the buffer is not deallocated but returned to the
 * caller on success. res_de is set accordingly.
 *
 * If cont is non-zero, raw_found continues with the entry after the one
 * res_bh/res_de point to.
 */
static int fatx_raw_scan_sector(struct super_block *sb,	int sector,
		const char *name, int name_length, int *number,
		loff_t *i_pos, struct buffer_head **res_bh,
		struct fatx_dir_entry **res_de )
{
	struct buffer_head *bh;
	struct fatx_dir_entry *data;
	int entry,start,done = 0;

	PRINTK("FATX: fatx_raw_scan_sector: sector=%08lX\n",(long)sector);
	
	if (!(bh = sb_bread(sb,sector))) {
		printk("FATX: fatx_raw_scan_sector: sb_bread failed\n");
		return -EIO;
	}
	data = (struct fatx_dir_entry *) bh->b_data;
	for (entry = 0; entry < FATX_SB(sb)->dir_per_block; entry++) {
		if (FATX_END_OF_DIR(&data[entry])) {
			//no more entries to look through...
			if(bh) brelse(bh);
			PRINTK("FATX: fatx_raw_scan_sector: END OF DIR\n");
			return -ENOENT;
		} else if (name) { //search for name
			done = 	(data[entry].name_length == name_length) &&
				!strncmp(data[entry].name,name,name_length);
		} else if (!i_pos) { /* count subdirectories */
			done = 0;
			if (!FATX_IS_FREE(&data[entry]) && (data[entry].attr & ATTR_DIR))
				(*number)++;
		} else if (number) { /* search for start cluster */
			done = !FATX_IS_FREE(&data[entry]) && 
				(CF_LE_L(data[entry].start) == *number);
		} else { /* search for free entry */
			done = FATX_IS_FREE(&data[entry]);
		}
		if (done) {
			if (i_pos)
				*i_pos = sector * FATX_SB(sb)->dir_per_block + entry;
			start = CF_LE_L(data[entry].start);
			if (!res_bh) {
				if(bh) brelse(bh);
			} else {
				*res_bh = bh;
				*res_de = &data[entry];
			}
			PRINTK("FATX: fatx_raw_scan_sector: found: start=%08lX\n",(long)start);
			return start;
		}
	}
	if(bh) brelse(bh);
	PRINTK("FATX: fatx_raw_scan_sector: entry not in sector %08lX\n",(long)sector);
	return -EAGAIN;
}

/*
 * raw_scan_root performs raw_scan_sector on the root directory until the
 * requested entry is found or the end of the directory is reached.
 */
static int fatx_raw_scan_root(struct super_block *sb, const char *name,
		int name_length, int *number, loff_t *i_pos,
		struct buffer_head **res_bh, struct fatx_dir_entry **res_de )
{
	int count,cluster;

	for (count = 0; count < FATX_SB(sb)->cluster_size; count++) {
		if ((cluster = fatx_raw_scan_sector(sb,FATX_SB(sb)->dir_start + count,
				       name,name_length,number,i_pos,res_bh,res_de)) >= 0)
			return cluster;
		if (cluster == -ENOENT) {
			//end of dir...act like all sectors scanned and !found
			PRINTK("FATX: fatx_raw_scan_root cluster %d\n",cluster);
			return cluster;
		}
	}
	
	PRINTK("FATX: fatx_raw_scan_root leave\n");

	return -ENOENT;
}

/*
 * raw_scan_nonroot performs raw_scan_sector on a non-root directory until the
 * requested entry is found or the end of the directory is reached.
 */
static int fatx_raw_scan_nonroot(struct super_block *sb, int start,
		const char *name, int name_length, int *number,
		loff_t *i_pos, struct buffer_head **res_bh,
		struct fatx_dir_entry **res_de )
{
	int count,cluster;
	
	PRINTK("FATX: fatx_raw_scan_nonroot: entered (start=%08lX)\n",(long)start);

	do {
		for (count = 0; count < FATX_SB(sb)->cluster_size; count++) {
			if ((cluster = fatx_raw_scan_sector(sb,FATX_SB(sb)->data_start + 
					(FATX_SB(sb)->cluster_size * (start - 2) ) + count,
			    name,name_length,number,i_pos,res_bh,res_de)) >= 0)
				return cluster;
			if (cluster == -ENOENT) {
				//EOD: act like all sectors scanned and !found
				return cluster;
			}
		}
		if (!(start = fatx_access(sb,start,-1))) {
			printk("FATX: fatx_raw_scan_nonroot: start sector %lX not in use\n",(long)start);
			fatx_fs_panic(sb,"FATX error");
			break;
		}
	}
	while (start != -1);
	return -ENOENT;
}

/*
 * Scans a directory for a given file (name points to its formatted name) or
 * for an empty directory slot (name is NULL). Returns an error code or zero.
 */
int fatx_scan(struct inode *dir, const char *name, int name_length,
		struct buffer_head **res_bh, struct fatx_dir_entry **res_de,
		loff_t *i_pos )
{
	int res;

	if (FATX_I(dir)->i_start)
		res = fatx_raw_scan_nonroot(dir->i_sb,FATX_I(dir)->i_start,name,name_length,NULL,i_pos,res_bh,res_de);
	else
		res = fatx_raw_scan_root(dir->i_sb,name,name_length,NULL,i_pos,res_bh,res_de);

	return res<0 ? res : 0;
}

/*
 * See if directory is empty
 */
int fatx_dir_empty(struct inode *dir)
{
	loff_t pos;
	struct buffer_head *bh;
	struct fatx_dir_entry *de;
	loff_t i_pos;
	int result = 0;

	pos = 0;
	bh = NULL;
	while (fatx_get_entry(dir,&pos,&bh,&de,&i_pos) > -1) {
		if (FATX_END_OF_DIR(de)) {
			break;
		}
		if (!FATX_IS_FREE(de)) {
			result = -ENOTEMPTY;
			break;
		}
	}
	if (bh)
		brelse(bh);

	return result;
}

/*
 * fatx_subdirs counts the number of sub-directories of dir. It can be run
 * on directories being created.
 */
int fatx_subdirs(struct inode *dir)
{
	int count;

	count = 0;
	if (dir->i_ino == FATX_ROOT_INO) {
		fatx_raw_scan_root(dir->i_sb,NULL,0,&count,NULL,NULL,NULL);
	} else {
		if ((dir->i_ino != FATX_ROOT_INO) && !FATX_I(dir)->i_start) {
			return 0; /* in mkdir */
		} else {
			fatx_raw_scan_nonroot(dir->i_sb,FATX_I(dir)->i_start,
			                      NULL,0,&count,NULL,NULL,NULL);
		}
	}
	return count;
}

int fatx_do_add_entry(
		struct inode *dir,
		struct buffer_head **bh,
		struct fatx_dir_entry **de,
		loff_t *i_pos)
{
	loff_t offset, curr;
	struct buffer_head *new_bh;

	offset = curr = 0;
	*bh = NULL;
	while (fatx_get_entry(dir,&curr,bh,de,i_pos) > -1) {
		if (FATX_IS_FREE(*de)) {
			PRINTK("FATX: fatx_do_add_entry: found free entry\n");
			return offset;
		}
		if (FATX_END_OF_DIR(*de)) {
			struct buffer_head *eod_bh = NULL;
			struct fatx_dir_entry *eod_de = NULL;
			loff_t eod_i_pos;
			
			PRINTK("FATX: fatx_do_add_entry: found EOD at %lX\n",(long)(*de));
			//make sure the next one isn't first in new cluster
			if (fatx_get_entry(dir,&curr,&eod_bh,&eod_de,&eod_i_pos) > -1) {
				//EOD in same cluster...find proper de and mark new EOD
				eod_de->name_length = 0xFF;
				mark_buffer_dirty(eod_bh);
				PRINTK("FATX: fatx_do_add_entry: marked new EOD at %lX\n",(long)eod_de);
				if(eod_bh) brelse(eod_bh);
			} else {
				//we will take the easy out...do nothing...
				//assume fat table used to indicate EOD
				//if this is wrong, need to fatx_extend_dir
				//making first entry in next cluster EOD
				printk("FATX: fatx_do_add_entry: EOD marked by FAT\n");
				printk("FATX: ...:offset=%08lX, curr=%08lX\n",
						(unsigned long)offset,(unsigned long)curr);
			}
			PRINTK("FATX: fatx_do_add_entry: using entry at %lX\n",(long)(*de));
			return offset;
		}
		offset = curr;
	}
	PRINTK("FATX: fatx_do_add_entry: need to extend dir\n");
	if (dir->i_ino == FATX_ROOT_INO) {
		printk("FATX: fatx_do_add_entry: but it's root dir...can't extend\n");
		return -ENOSPC;
	}
	new_bh = fatx_extend_dir(dir);
	if (!new_bh) {
		PRINTK("FATX: fatx_do_add_entry: fatx_extend_dir failed...no space?\n");
		return -ENOSPC;
	}
	if(new_bh) brelse(new_bh);
	fatx_get_entry(dir,&curr,bh,de,i_pos);
	(*de)[1].name_length = 0xFF;
	PRINTK("FATX: fatx_do_add_entry: using entry at %ld\n",(long)offset);
	return offset;
}

int fatx_new_dir(struct inode *dir, struct inode *parent)
{
	struct buffer_head *bh;
	struct fatx_dir_entry *de;

	if ((bh = fatx_extend_dir(dir)) == NULL) {
		printk("FATX: fatx_new_dir: failed to get new cluster...no space?\n");
		return -ENOSPC;
	}
	/* zeroed out, so... */
	de = (struct fatx_dir_entry*)&bh->b_data[0];
	de[0].attr = de[1].attr = ATTR_DIR;
	de[0].name_length = 0xFF; //end of dir marker
        de[0].start = CT_LE_W(FATX_I(dir)->i_logstart);
	de[1].start = CT_LE_W(FATX_I(parent)->i_logstart);	
	mark_buffer_dirty(bh);
	if(bh) brelse(bh);
	dir->i_atime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(dir);

	return 0;
}

// sure to hope this is correct...
int fatx_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct inode *tmpi;
	struct super_block *sb = inode->i_sb;
	struct fatx_dir_entry *de;
	struct buffer_head *bh;
	loff_t i_pos;
	int inum;
	loff_t cpos = 0;	//file position (dir position)
	int offset = 0;		//cpos offset for root dir handling
	int entry = 0;		//next filldir entry location

	PRINTK("FATX: fatx_readdir entered\n");
	
	cpos = filp->f_pos;
	
	if (cpos == 0) {
		if (filldir(dirent,".",1,entry++,inode->i_ino,DT_DIR)<0) {
			printk("\nFATX: fatx_readdir exiting in root defaults\n");
			return 0;
		}
		cpos += 1 << FATX_DIR_BITS;
	}
	
	if (cpos == 1 << FATX_DIR_BITS) {
		if (filldir(dirent,"..",2,entry++,
		            filp->f_dentry->d_parent->d_inode->i_ino,DT_DIR)<0) {
			printk("\nFATX: fatx_readdir exiting in root defaults\n");
			filp->f_pos = 1 << FATX_DIR_BITS;
			return 0;
		}
		cpos += 1 << FATX_DIR_BITS;
	}
	
	offset = 2 << FATX_DIR_BITS;
	cpos -= offset;

 	bh = NULL;

	while(fatx_get_entry(inode,&cpos,&bh,&de,&i_pos) != -1) {
		if (FATX_END_OF_DIR(de)) {
			PRINTK("FATX: entry %ld marked as END OF DIR\n",(long)(cpos >> FATX_DIR_BITS));
			cpos -= 1 << FATX_DIR_BITS; // make sure it comes back to here if re-entered
			break;		//done...end of dir.
		}
		
		if (FATX_IS_FREE(de)) {
			PRINTK("FATX: entry %ld marked as FREE\n",(long)(cpos >> FATX_DIR_BITS));
			continue;
		}

		tmpi = fatx_iget(sb, i_pos);
		if (tmpi) {
			inum = tmpi->i_ino;
			iput(tmpi);
		} else {
			inum = iunique(sb, FATX_ROOT_INO);
		}

		if (filldir(dirent,de->name,de->name_length,entry++,inum,
		            (de->attr & ATTR_DIR) ? DT_DIR : DT_REG ) < 0 ) {
			break;
		}
		PRINTK("\nFATX: fatx_readdir: dir entry %3d: ",(int)entry);
		fatx_printname(de->name,de->name_length);
		PRINTK("\n");
	}

	filp->f_pos = cpos + offset;		
	if (bh)
		brelse(bh);
	
	PRINTK("\nFATX: fatx_readdir leaving\n");
	
	return 0;
}

struct file_operations fatx_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= fatx_readdir,
	.ioctl		= NULL,
	.fsync		= file_fsync,
};

/* This assumes that size of cluster is above the 32*slots */

int fatx_add_entries(struct inode *dir,int slots, struct buffer_head **bh,
		  struct fatx_dir_entry **de, loff_t *i_pos)
{
	loff_t offset, curr;
	int row;
	struct buffer_head *new_bh;

	offset = curr = 0;
	*bh = NULL;
	row = 0;
	while (fatx_get_entry(dir,&curr,bh,de,i_pos) > -1) {
		if (IS_FREE((*de)->name)) {
			if (++row == slots)
				return offset;
		} else {
			row = 0;
			offset = curr;
		}
	}
	if (dir->i_ino == FATX_ROOT_INO) 
		return -ENOSPC;
	new_bh = fatx_extend_dir(dir);
	if (!new_bh)
		return -ENOSPC;
	if(new_bh) brelse(new_bh);
	do fatx_get_entry(dir,&curr,bh,de,i_pos); while (++row<slots);
	return offset;
}
