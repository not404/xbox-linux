/*
 *  linux/fs/fatx/misc.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  22/11/2000 - Fixed fatx_date_unix2dos for dates earlier than 01/01/1980
 *		 and date_dos2unix for date==0 by Igor Zhbanov(bsg@uniyar.ac.ru)
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fatx_fs.h>
#include <linux/buffer_head.h>

extern unsigned int fatx_debug;

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

/*
 * fatx_fs_panic reports a severe file system problem and sets the file system
 * read-only. The file system can be made writable again by remounting it.
 */
void fatx_fs_panic(struct super_block *s, const char *fmt, ...)
{
	va_list args;

	printk(KERN_ERR "FATX: Filesystem panic (dev %s)\n", s->s_id);

	printk(KERN_ERR "    ");
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
	printk("\n");

	if (!(s->s_flags & MS_RDONLY)) {
		s->s_flags |= MS_RDONLY;
		printk(KERN_ERR "    File system has been set read-only\n");
	}
}

void lock_fatx(struct super_block *sb)
{
	down(&(FATX_SB(sb)->fatx_lock));
}

void unlock_fatx(struct super_block *sb)
{
	up(&(FATX_SB(sb)->fatx_lock));
}

/*
 * fatx_add_cluster tries to allocate a new cluster and adds it to the
 * file represented by inode.
 */
__s64 fatx_add_cluster(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	__s64 ret;
	int count, limit, new_dclus, new_fclus, last;
	int cluster_bits = sbi->cluster_bits;

	PRINTK("FATX: %s \n", __FUNCTION__);
	/*
	 * We must locate the last cluster of the file to add this new
	 * one (new_dclus) to the end of the link list (the FAT).
	 *
	 * In order to confirm that the cluster chain is valid, we
	 * find out EOF first.
	 */
	last = new_fclus = 0;
	if (FATX_I(inode)->i_start) {
		__s64 ret; 
		int fclus, dclus;

		ret = fatx_get_cluster(inode, FAT_ENT_EOF, &fclus, &dclus);
		if (ret < 0) {
			return ret;
		}
		new_fclus = fclus + 1;
		last = dclus;
	}

	/* find free FAT entry */
	lock_fatx(sb);

	if (sbi->free_clusters == 0) {
		unlock_fatx(sb);
		return -ENOSPC;
	}

	limit = sbi->max_cluster;
	new_dclus = sbi->prev_free + 1;
	for (count = FAT_START_ENT; count < limit; count++, new_dclus++) {
		new_dclus = new_dclus % limit;
		if (new_dclus < FAT_START_ENT)
			new_dclus = FAT_START_ENT;

		ret = fatx_access(sb, new_dclus, -1);
		if (ret < 0) {
			unlock_fatx(sb);
			return ret;
		} else if (ret == FAT_ENT_FREE)
			break;
	}
	if (count >= limit) {
		sbi->free_clusters = 0;
		unlock_fatx(sb);
		return -ENOSPC;
	}

	ret = fatx_access(sb, new_dclus, FAT_ENT_EOF);
	if (ret < 0) {
		unlock_fatx(sb);
		return ret;
	}

	sbi->prev_free = new_dclus;
	if (sbi->free_clusters != -1)
		sbi->free_clusters--;

	unlock_fatx(sb);

	/* add new one to the last of the cluster chain */
	if (last) {
		ret = fatx_access(sb, last, new_dclus);
		if (ret < 0)
			return ret;
	} else {
		FATX_I(inode)->i_start = new_dclus;
		FATX_I(inode)->i_logstart = new_dclus;
		mark_inode_dirty(inode);
	}
	if (new_fclus != (inode->i_blocks >> (cluster_bits - 9))) {
		fatx_fs_panic(sb, "clusters badly computed (%d != %lu)",
			new_fclus, inode->i_blocks >> (cluster_bits - 9));
		fatx_cache_inval_inode(inode);
	}
	inode->i_blocks += sbi->cluster_size >> 9;

	return new_dclus;
}

extern struct timezone sys_tz;

/* Linear day numbers of the respective 1sts in non-leap years. */
static int day_n[] = {
   /* Jan  Feb  Mar  Apr   May  Jun  Jul  Aug  Sep  Oct  Nov  Dec */
	0,  31,  59,  90,  120, 151, 181, 212, 243, 273, 304, 334, 0, 0, 0, 0
};

int fatx_date_dos2unix(unsigned short time,unsigned short date)
{
	int month,year,secs,days;

	/* first subtract and mask after that... Otherwise, if
	   date == 0, bad things happen */
	month=  ((date >> 5) - 1) & 15;
	year =  date >> 9;
	days =  (date & 31)-1+day_n[month]+(year/4)+(year+30)*365;
	//skipped and current leap years
	days += ((year & 3) == 0 && month < 2 ? 0 : 1) + 7; 
	
	secs =  (time & 31)*2;		//seconds into curr minute
	secs += 60*((time >> 5) & 63);	//minutes into curr hour
	secs += 3600*(time >> 11);	//hours into curr day
	secs += 86400*days;		//days (from 1.1.70)
	
	secs += sys_tz.tz_minuteswest*60;
	return secs;
}

void fatx_date_unix2dos(int unix_date,unsigned short *time,
    unsigned short *date)
{
	int day,year,nl_day,month;

	unix_date -= sys_tz.tz_minuteswest*60;

	/* bound low end at Jan 1 GMT 00:00:00 2000. */
	if (unix_date < ((30 * 365) + 7) * 24 * 60 * 60) {
		unix_date = ((30 * 365) + 7) * 24 * 60 * 60;
	}
		
	*time = (unix_date % 60)/2 + 			//seconds
		(((unix_date/60) % 60) << 5) +		//minutes
		(((unix_date/3600) % 24) << 11);	//hours
	
	day = unix_date/86400-(30 * 365 + 7);		//days (from 1.1.2000)
	year = day/365;
	if ((year+3)/4+365*year > day) year--;
	day -= (year+3)/4+365*year;
	if (day == 59 && !(year & 3)) {
		nl_day = day;
		month = 2;
	}
	else {
		nl_day = (year & 3) || day <= 59 ? day : day-1;
		for (month = 0; month < 12; month++)
			if (day_n[month] > nl_day) break;
	}
	*date = nl_day-day_n[month-1]+1+(month << 5)+(year << 9);
}


/* Convert linear UNIX date to a MS-DOS time/date pair. */
/*
void fatx_date_unix2dos(int unix_date, __le16 *time, __le16 *date)
{
	int day, year, nl_day, month;

	unix_date -= sys_tz.tz_minuteswest*60;

	if (unix_date < 315532800)
		unix_date = 315532800;

	*time = cpu_to_le16((unix_date % 60)/2+(((unix_date/60) % 60) << 5)+
	    (((unix_date/3600) % 24) << 11));
	day = unix_date/86400-3652;
	year = day/365;
	if ((year+3)/4+365*year > day)
		year--;
	day -= (year+3)/4+365*year;
	if (day == 59 && !(year & 3)) {
		nl_day = day;
		month = 2;
	} else {
		nl_day = (year & 3) || day <= 59 ? day : day-1;
		for (month = 0; month < 12; month++) {
			if (day_n[month] > nl_day)
				break;
		}
	}
	*date = cpu_to_le16(nl_day-day_n[month-1]+1+(month << 5)+(year << 9));
}
*/

EXPORT_SYMBOL(fatx_date_unix2dos);

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

int fatx__get_entry(struct inode *dir, loff_t *pos, struct buffer_head **bh,
		   struct fatx_dir_entry **de, loff_t *i_pos)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	sector_t phys, iblock;
	loff_t offset;
	int err;

	PRINTK("FATX: %s \n", __FUNCTION__);
next:
	offset = *pos;
	if (*bh)
		brelse(*bh);

	*bh = NULL;
	iblock = *pos >> sb->s_blocksize_bits;
//	PRINTK("FATX: fatx__get_entry iblock %ld pos %lld sb->s_blocksize_bits %d\n", iblock, *pos, sb->s_blocksize_bits);
	err = fatx_bmap(dir, iblock, &phys);
	if (err || !phys)
		return -1;

	*bh = sb_bread(sb, phys);
	if (*bh == NULL) {
		printk(KERN_ERR "FATX: Directory bread(block %llu) failed\n",
		       (unsigned long long)phys);
		*pos = (iblock + 1) << sb->s_blocksize_bits;
		goto next;
	}

	offset &= sb->s_blocksize - 1;
	*pos += sizeof(struct fatx_dir_entry);
	*de = (struct fatx_dir_entry *)((*bh)->b_data + offset);
	*i_pos = ((loff_t)phys << sbi->dir_per_block_bits) + (offset >> FATX_DIR_BITS);
//	PRINTK("FATX: fatx__get_entry iblock %ld pos %lld sb->s_blocksize_bits %d sbi->dir_per_block_bits %d offset %lld\n", iblock, *pos, sb->s_blocksize_bits, sbi->dir_per_block_bits, offset);

	return 0;
}

EXPORT_SYMBOL(fatx_get_entry);
