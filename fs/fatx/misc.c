/*
 *  linux/fs/fatx/misc.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  22/11/2000 - Fixed fatx_date_unix2dos for dates earlier than 01/01/1980
 *		 and date_dos2unix for date==0 by Igor Zhbanov(bsg@uniyar.ac.ru)
 *
 *  FATX port 2005 by Edgar Hucek
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fatx_fs.h>
#include <linux/buffer_head.h>

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

EXPORT_SYMBOL(fatx_fs_panic);

/*
 * fatx_chain_add() adds a new cluster to the chain of clusters represented
 * by inode.
 */
__s64 fatx_chain_add(struct inode *inode, int new_dclus, int nr_cluster)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	__s64 ret; 
	int new_fclus, last;

	/*
	 * We must locate the last cluster of the file to add this new
	 * one (new_dclus) to the end of the link list (the FATX).
	 */
	last = new_fclus = 0;
	if (FATX_I(inode)->i_start) {
		int fclus, dclus;

		PRINTK("FATX: %s new_fclus %d  inode->i_blocks %ld sbi->cluster_bits %d\n",
			__FUNCTION__, new_fclus,  inode->i_blocks, sbi->cluster_bits);
		ret = fatx_get_cluster(inode, FAT_ENT_EOF, &fclus, &dclus);
		if (ret < 0)
			return ret;
		new_fclus = fclus + 1;
		last = dclus;
	}

	/* add new one to the last of the cluster chain */
	if (last) {
		struct fatx_entry fatxent;

		fatxent_init(&fatxent);
		ret = fatx_ent_read(inode, &fatxent, last);
		if (ret >= 0) {
			int wait = inode_needs_sync(inode);
			ret = fatx_ent_write(inode, &fatxent, new_dclus, wait);
			fatxent_brelse(&fatxent);
		}
		if (ret < 0)
			return ret;
//		fatx_cache_add(inode, new_fclus, new_dclus);
	} else {
		FATX_I(inode)->i_start = new_dclus;
		FATX_I(inode)->i_logstart = new_dclus;
		/*
		 * Since generic_osync_inode() synchronize later if
		 * this is not directory, we don't here.
		 */
		if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode)) {
			ret = fatx_sync_inode(inode);
			if (ret)
				return ret;
		} else
			mark_inode_dirty(inode);
	}
	if (new_fclus != (inode->i_blocks >> (sbi->cluster_bits - 9))) {
		fatx_fs_panic(sb, "clusters badly computed (%d != %lu)",
			new_fclus, inode->i_blocks >> (sbi->cluster_bits - 9));
		fatx_cache_inval_inode(inode);
	}
	inode->i_blocks += nr_cluster << (sbi->cluster_bits - 9);

	return 0;
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

/* Convert linear UNIX date to a MS-DOS time/date pair. */
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

EXPORT_SYMBOL(fatx_date_unix2dos);

int fatx_sync_bhs(struct buffer_head **bhs, int nr_bhs)
{
	int i, e, err = 0;

	for (i = 0; i < nr_bhs; i++) {
		lock_buffer(bhs[i]);
		if (test_clear_buffer_dirty(bhs[i])) {
			get_bh(bhs[i]);
			bhs[i]->b_end_io = end_buffer_write_sync;
			e = submit_bh(WRITE, bhs[i]);
			if (!err && e)
				err = e;
		} else
			unlock_buffer(bhs[i]);
	}
	for (i = 0; i < nr_bhs; i++) {
		wait_on_buffer(bhs[i]);
		if (buffer_eopnotsupp(bhs[i])) {
			clear_buffer_eopnotsupp(bhs[i]);
			err = -EOPNOTSUPP;
		} else if (!err && !buffer_uptodate(bhs[i]))
			err = -EIO;
	}
	return err;
}

EXPORT_SYMBOL(fatx_sync_bhs);
