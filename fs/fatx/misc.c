/*
 *  linux/fs/fatx/misc.c
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#include <linux/fatx_fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/types.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

/*
 * fatx_fs_panic reports a severe file system problem and sets the file system
 * read-only. The file system can be made writable again by remounting it.
 */

void fatx_fs_panic(struct super_block *s,const char *msg)
{
	int not_ro;

	not_ro = !(s->s_flags & MS_RDONLY);
	if (not_ro) s->s_flags |= MS_RDONLY;
	printk("Filesystem panic (dev %s).\n  %s\n", s->s_id, msg);
	if (not_ro)
		printk("  File system has been set read-only\n");
}

static int day_n[] = {  0, 31, 59, 90,120,151,181,212,243,273,304,334,0,0,0,0 };
		/*    Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec */

extern struct timezone sys_tz;

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


/*
 * fat_add_cluster tries to allocate a new cluster and adds it to the
 * file represented by inode.
 */
int fatx_add_cluster(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	int count, nr, limit, last, curr, file_cluster;
	int cluster_size = FATX_SB(sb)->cluster_size;
	int res = -ENOSPC;
	
	lock_fatx(sb);

	if (FATX_SB(sb)->free_clusters == 0) {
		unlock_fatx(sb);
		return res;
	}
	limit = FATX_SB(sb)->clusters;
	nr = limit; /* to keep GCC happy */
	for (count = 0; count < limit; count++) {
		nr = ((count + FATX_SB(sb)->prev_free) % limit) + 2;
		if (fatx_access(sb, nr, -1) == 0)
			break;
	}
	if (count >= limit) {
		FATX_SB(sb)->free_clusters = 0;
		unlock_fatx(sb);
		return res;
	}
	
	FATX_SB(sb)->prev_free = (count + FATX_SB(sb)->prev_free + 1) % limit;
	fatx_access(sb, nr, EOF_FAT(sb));
	if (FATX_SB(sb)->free_clusters != -1)
		FATX_SB(sb)->free_clusters--;
	
	unlock_fatx(sb);
	
	/* We must locate the last cluster of the file to add this
	   new one (nr) to the end of the link list (the FAT).
	   
	   Here file_cluster will be the number of the last cluster of the
	   file (before we add nr).
	   
	   last is the corresponding cluster number on the disk. We will
	   use last to plug the nr cluster. We will use file_cluster to
	   update the cache.
	*/
	last = file_cluster = 0;
	if ((curr = FATX_I(inode)->i_start) != 0) {
		fatx_cache_lookup(inode, INT_MAX, &last, &curr);
		file_cluster = last;
		while (curr && curr != -1){
			file_cluster++;
			if (!(curr = fatx_access(sb, last = curr,-1))) {
				fatx_fs_panic(sb, "File without EOF");
				return res;
			}
		}
	}
	if (last) {
		fatx_access(sb, last, nr);
		fatx_cache_add(inode, file_cluster, nr);
	} else {
		FATX_I(inode)->i_start = nr;
		FATX_I(inode)->i_logstart = nr;
		mark_inode_dirty(inode);
	}
	if (file_cluster
	    != inode->i_blocks / cluster_size / (sb->s_blocksize / 512)) {
		printk ("file_cluster badly computed!!! %d <> %ld\n",
			file_cluster,
			inode->i_blocks / cluster_size / (sb->s_blocksize / 512));
		fatx_cache_inval_inode(inode);
	}
	inode->i_blocks += (1 << FATX_SB(sb)->cluster_bits) / 512;
	return nr;
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

struct buffer_head *fatx_extend_dir(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	int nr, sector, last_sector;
	struct buffer_head *bh, *res = NULL;
	int cluster_size = FATX_SB(sb)->cluster_size;

	if (inode->i_ino == FATX_ROOT_INO)
			return res;

	nr = fatx_add_cluster(inode);
	if (nr < 0)
		return res;
	
	sector = FATX_SB(sb)->data_start + (nr - 2) * cluster_size;
	last_sector = sector + cluster_size;
	for ( ; sector < last_sector; sector++) {
#ifdef DEBUG
		printk("zeroing sector %d\n", sector);
#endif
		if (!(bh = sb_getblk(sb, sector)))
			printk("getblk failed\n");
		else {
			memset(bh->b_data, 0xFF, sb->s_blocksize);
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
			if (!res)
				res = bh;
			else
				if(bh) brelse(bh);
		}
	}
	if (inode->i_size & (sb->s_blocksize - 1)) {
		fatx_fs_panic(sb, "Odd directory size");
		inode->i_size = (inode->i_size + sb->s_blocksize)
			& ~(sb->s_blocksize - 1);
	}
	inode->i_size += 1 << FATX_SB(sb)->cluster_bits;
	FATX_I(inode)->mmu_private += 1 << FATX_SB(sb)->cluster_bits;
	mark_inode_dirty(inode);

	return res;
}

/* Returns the inode number of the directory entry at offset pos. If bh is
   non-NULL, it is brelse'd before. Pos is incremented. The buffer header is
   returned in bh.
   AV. Most often we do it item-by-item. Makes sense to optimize.
   AV. OK, there we go: if both bh and de are non-NULL we assume that we just
   AV. want the next entry (took one explicit de=NULL in vfat/namei.c).
   AV. It's done in fatx_get_entry() (inlined), here the slow case lives.
   AV. Additionally, when we return -1 (i.e. reached the end of directory)
   AV. we make bh NULL. 
 */

int fatx_get_entry(struct inode *dir, loff_t *pos,struct buffer_head **bh,
    struct fatx_dir_entry **de, loff_t *i_pos)
{
	struct super_block *sb = dir->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	int sector, offset;

	while (1) {
		offset = *pos;
		PRINTK("get_entry offset %d\n",offset);
		if (*bh)
			if(*bh) brelse(*bh);
		*bh = NULL;
		if ((sector = fatx_bmap(dir,offset >> sb->s_blocksize_bits)) == -1)
			return -1;
		PRINTK("FATX: get_entry sector %d %p\n",sector,*bh);
		PRINTK("FATX: get_entry sector apres brelse\n");
		if (!sector)
			return -1; /* beyond EOF */
		*pos += sizeof(struct fatx_dir_entry);
		if (!(*bh = sb_bread(sb, sector))) {
			printk("Directory sread (sector 0x%x) failed\n",sector);
			continue;
		}
		PRINTK("FATX: get_entry apres sread\n");

		offset &= sb->s_blocksize - 1;
		*de = (struct fatx_dir_entry *) ((*bh)->b_data + offset);
		*i_pos = (sector << sbi->dir_per_block_bits) + (offset >> FATX_DIR_BITS);

		return 0;
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

