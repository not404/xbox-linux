/*
 *  linux/fs/fatx/cache.c
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#include <linux/fatx_fs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/buffer_head.h>

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

static struct fatx_cache *fatx_cache,cache[FATX_CACHE];
static spinlock_t fatx_cache_lock = SPIN_LOCK_UNLOCKED;

int fatx_access(struct super_block *sb,int nr,int new_value)
{
	struct buffer_head *bh, *bh2, *c_bh, *c_bh2;
	unsigned char *p_first, *p_last;
	int copy, first = 0, last = 0, next, b;

	next = 0;

	if ((unsigned) (nr-2) >= FATX_SB(sb)->clusters)
		return 0;
	if (FATX_SB(sb)->fat_bits == 32) {
		first = last = nr*4;
	} else if (FATX_SB(sb)->fat_bits == 16) {
		first = last = nr*2;
	}

	b = FATX_SB(sb)->fat_start + (first >> sb->s_blocksize_bits);
	if (!(bh = sb_bread(sb, b))) {
		PRINTK("FATX: bread in fatx_access failed\n");
		return 0;
	}
	if ((first >> sb->s_blocksize_bits) == (last >> sb->s_blocksize_bits)) {
		bh2 = bh;
	} else {
		if (!(bh2 = sb_bread(sb, b+1))) {
			if(bh) brelse(bh);
			PRINTK("FATX: 2nd bread in fatx_access failed\n");
			return 0;
		}
	}
	if (FATX_SB(sb)->fat_bits == 32) {
		p_first = p_last = NULL; /* GCC needs that stuff */
		next = CF_LE_L(((__u32 *) bh->b_data)[(first & (sb->s_blocksize - 1)) >> 2]);
		next &= 0xffffffff;
		if (next >= EOC_FAT32) next = -1;
	} else if (FATX_SB(sb)->fat_bits == 16) {
		p_first = p_last = NULL; /* GCC needs that stuff */
		next = CF_LE_W(((__u16 *) bh->b_data)[(first & (sb->s_blocksize - 1)) >> 1]);
		if (next >= EOC_FAT16) next = -1;
	}
	PRINTK("FATX: fatx_access: 0x%x, nr=0x%x, first=0x%x, next=0x%x\n", b, nr, first, next);
	if (new_value != -1) {
		if (FATX_SB(sb)->fat_bits == 32) {
			((__u32 *)bh->b_data)[(first & (sb->s_blocksize - 1)) >> 2]
				= CT_LE_L(new_value);
		} else if (FATX_SB(sb)->fat_bits == 16) {
			((__u16 *)bh->b_data)[(first & (sb->s_blocksize - 1)) >> 1]
				= CT_LE_W(new_value);
		}
		mark_buffer_dirty(bh);
		for (copy = 1; copy < FATX_SB(sb)->fats; copy++) {
			b = FATX_SB(sb)->fat_start + (first >> sb->s_blocksize_bits)
				+ FATX_SB(sb)->fat_length * copy;
			if (!(c_bh = sb_bread(sb, b)))
				break;
			if (bh != bh2) {
				if (!(c_bh2 = sb_bread(sb, b+1))) {
					if(c_bh) brelse(c_bh);
					break;
				}
				memcpy(c_bh2->b_data, bh2->b_data, sb->s_blocksize);
				mark_buffer_dirty(c_bh2);
				if(c_bh2) brelse(c_bh2);
			}
			memcpy(c_bh->b_data, bh->b_data, sb->s_blocksize);
			mark_buffer_dirty(c_bh);
			if(c_bh) brelse(c_bh);
		}
	}
	if(bh) brelse(bh);
	if (bh != bh2)
		if(bh2) brelse(bh2);
	return next;
}

void fatx_cache_init(void)
{
	static int initialized = 0;
	int count;

	spin_lock(&fatx_cache_lock);
	if (initialized) {
		spin_unlock(&fatx_cache_lock);
		return;
	}
	fatx_cache = &cache[0];
	for (count = 0; count < FATX_CACHE; count++) {
		cache[count].device = 0;
		cache[count].next = count == FATX_CACHE-1 ? NULL :
		    &cache[count+1];
	}
	initialized = 1;
	spin_unlock(&fatx_cache_lock);
}


void fatx_cache_lookup(struct inode *inode,int cluster,int *f_clu,int *d_clu)
{
	struct fatx_cache *walk;
	int first = FATX_I(inode)->i_start;

	if (!first)
		return;
	spin_lock(&fatx_cache_lock);
	for (walk = fatx_cache; walk; walk = walk->next)
		if (inode->i_rdev == walk->device
		    && walk->start_cluster == first
		    && walk->file_cluster <= cluster
		    && walk->file_cluster > *f_clu) {
			*d_clu = walk->disk_cluster;
#ifdef DEBUG
printk("cache hit: %d (%d)\n",walk->file_cluster,*d_clu);
#endif
			if ((*f_clu = walk->file_cluster) == cluster) { 
				spin_unlock(&fatx_cache_lock);
				return;
			}
		}
	spin_unlock(&fatx_cache_lock);
#ifdef DEBUG
printk("cache miss\n");
#endif
}


#ifdef DEBUG
static void list_cache(void)
{
	struct fatx_cache *walk;

	for (walk = fatx_cache; walk; walk = walk->next) {
		if (walk->device)
			printk("<%s,%d>(%d,%d) ", kdevname(walk->device),
			       walk->start_cluster, walk->file_cluster,
			       walk->disk_cluster);
		else printk("-- ");
	}
	printk("\n");
}
#endif


void fatx_cache_add(struct inode *inode,int f_clu,int d_clu)
{
	struct fatx_cache *walk,*last;
	int first = FATX_I(inode)->i_start;

	last = NULL;
	spin_lock(&fatx_cache_lock);
	for (walk = fatx_cache; walk->next; walk = (last = walk)->next)
		if (walk->device == inode->i_rdev
		    && walk->start_cluster == first
		    && walk->file_cluster == f_clu) {
			if (walk->disk_cluster != d_clu) {
				printk("FAT cache corruption inode=%ld\n",
					inode->i_ino);
				spin_unlock(&fatx_cache_lock);
				fatx_cache_inval_inode(inode);
				return;
			}
			/* update LRU */
			if (last == NULL) {
				spin_unlock(&fatx_cache_lock);
				return;
			}
			last->next = walk->next;
			walk->next = fatx_cache;
			fatx_cache = walk;
#ifdef DEBUG
list_cache();
#endif
			spin_unlock(&fatx_cache_lock);
			return;
		}
	walk->device = inode->i_rdev;
	walk->start_cluster = first;
	walk->file_cluster = f_clu;
	walk->disk_cluster = d_clu;
	last->next = NULL;
	walk->next = fatx_cache;
	fatx_cache = walk;
	spin_unlock(&fatx_cache_lock);
#ifdef DEBUG
list_cache();
#endif
}


/* Cache invalidation occurs rarely, thus the LRU chain is not updated. It
   fixes itself after a while. */

void fatx_cache_inval_inode(struct inode *inode)
{
	struct fatx_cache *walk;
	int first = FATX_I(inode)->i_start;

	spin_lock(&fatx_cache_lock);
	for (walk = fatx_cache; walk; walk = walk->next)
		if (walk->device == inode->i_rdev
		    && walk->start_cluster == first)
			walk->device = 0;
	spin_unlock(&fatx_cache_lock);
}


void fatx_cache_inval_dev(dev_t device)
{
	struct fatx_cache *walk;

	spin_lock(&fatx_cache_lock);
	for (walk = fatx_cache; walk; walk = walk->next)
		if (walk->device == device)
			walk->device = 0;
	spin_unlock(&fatx_cache_lock);
}


int fatx_get_cluster(struct inode *inode,int cluster)
{
	int nr,count;

	if (!(nr = FATX_I(inode)->i_start)) return 0;
	if (!cluster) return nr;
	count = 0;
	for (fatx_cache_lookup(inode,cluster,&count,&nr); count < cluster;
	    count++) {
		if ((nr = fatx_access(inode->i_sb,nr,-1)) == -1) return 0;
		if (!nr) return 0;
	}
	fatx_cache_add(inode,cluster,nr);
	return nr;
}

unsigned long fatx_bmap(struct inode *inode,unsigned long sector)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	unsigned long cluster, offset, last_block;

	if ((inode->i_ino == FATX_ROOT_INO || (S_ISDIR(inode->i_mode) &&
	     !FATX_I(inode)->i_start))) {
		if (sector >= sbi->dir_entries >> sbi->dir_per_block_bits)
			return 0;
		return sector + sbi->dir_start;
	}
	
	last_block = (FATX_I(inode)->mmu_private + (sb->s_blocksize - 1))
		>> sb->s_blocksize_bits;
	if (sector >= last_block)
		return 0;

	cluster = sector / sbi->cluster_size;
	offset  = sector % sbi->cluster_size;
	if (!(cluster = fatx_get_cluster(inode, cluster)))
		return 0;

	return (cluster - 2) * sbi->cluster_size + sbi->data_start + offset;
}


/* Free all clusters after the skip'th cluster. Doesn't use the cache,
   because this way we get an additional sanity check. */

int fatx_free(struct inode *inode,int skip)
{
	int nr,last;

	if (!(nr = FATX_I(inode)->i_start)) return 0;
	last = 0;
	while (skip--) {
		last = nr;
		if ((nr = fatx_access(inode->i_sb,nr,-1)) == -1) return 0;
		if (!nr) {
			printk("fatx_free: skipped EOF\n");
			return -EIO;
		}
	}
	if (last) {
		fatx_access(inode->i_sb,last,EOF_FAT(inode->i_sb));
		fatx_cache_inval_inode(inode);
	} else {
		fatx_cache_inval_inode(inode);
		FATX_I(inode)->i_start = 0;
		FATX_I(inode)->i_logstart = 0;
		mark_inode_dirty(inode);
	}
	lock_fatx(inode->i_sb);
	while (nr != -1) {
		if (!(nr = fatx_access(inode->i_sb,nr,0))) {
			fatx_fs_panic(inode->i_sb,"fatx_free: deleting beyond EOF");
			break;
		}
		if (FATX_SB(inode->i_sb)->free_clusters != -1) {
			FATX_SB(inode->i_sb)->free_clusters++;
		}
		inode->i_blocks -= (1 << FATX_SB(inode->i_sb)->cluster_bits) / 512;
	}
	unlock_fatx(inode->i_sb);
	return 0;
}
