/*
 *  linux/fs/fatx/inode.c
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

#include <linux/module.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/seq_file.h>

#include <linux/fatx_fs.h>
#include <linux/fatx_fs_sb.h>

#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <asm/unaligned.h>


#define CONFIG_NLS_DEFAULT "iso8859-15"

#define FAT_HASH_BITS   8
#define FAT_HASH_SIZE    (1UL << FAT_HASH_BITS)
#define FAT_HASH_MASK    (FAT_HASH_SIZE-1)

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)

static int fatx_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page,fatx_get_block, wbc);
}

static int fatx_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page,fatx_get_block);
}

static int fatx_prepare_write(struct file *file, struct page *page, unsigned from, unsigned to)
{
	return cont_prepare_write(page,from,to,fatx_get_block,
		&FATX_I(page->mapping->host)->mmu_private);
}

static sector_t _fatx_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block,fatx_get_block);
}

static int fatx_commit_write(struct file *file, struct page *page, unsigned from, unsigned to)
{
	kunmap(page);
	return generic_commit_write(file, page, from, to);
}

struct address_space_operations fatx_aops = {
	.readpage	= fatx_readpage,
	.writepage	= fatx_writepage,
	.sync_page	= block_sync_page,
	.prepare_write	= fatx_prepare_write,
	.commit_write	= fatx_commit_write,
	.bmap		= _fatx_bmap
};

void fatx_put_super(struct super_block *sb)
{
	fatx_cache_inval_dev(sb->s_dev);
//	set_blocksize(sb->s_dev,BLOCK_SIZE);
        if (FATX_SB(sb)->nls_io) {
		unload_nls(FATX_SB(sb)->nls_io);
		FATX_SB(sb)->nls_io = NULL;
	}
}

static struct list_head fatx_inode_hashtable[FAT_HASH_SIZE];
spinlock_t fatx_inode_lock = SPIN_LOCK_UNLOCKED;

void fatx_hash_init(void)
{
	int i;
	for(i = 0; i < FAT_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&fatx_inode_hashtable[i]);
	}
}

static inline unsigned long fatx_hash(struct super_block *sb, int i_pos)
{
	unsigned long tmp = (unsigned long)i_pos | (unsigned long) sb;
	tmp = tmp + (tmp >> FAT_HASH_BITS) + (tmp >> FAT_HASH_BITS * 2);
	return tmp & FAT_HASH_MASK;
}

void fatx_attach(struct inode *inode, loff_t i_pos)
{
	spin_lock(&fatx_inode_lock);
	FATX_I(inode)->i_pos = i_pos;
	list_add(&FATX_I(inode)->i_fat_hash,
		fatx_inode_hashtable + fatx_hash(inode->i_sb, i_pos));
	spin_unlock(&fatx_inode_lock);
}

void fatx_detach(struct inode *inode)
{
	spin_lock(&fatx_inode_lock);
	FATX_I(inode)->i_pos = 0;
	list_del(&FATX_I(inode)->i_fat_hash);
	INIT_LIST_HEAD(&FATX_I(inode)->i_fat_hash);
	spin_unlock(&fatx_inode_lock);
}

struct inode *fatx_iget(struct super_block *sb, loff_t i_pos)
{
	struct list_head *p = fatx_inode_hashtable + fatx_hash(sb, i_pos);
	struct list_head *walk;
	struct fatx_inode_info *i;
	struct inode *inode = NULL;

	spin_lock(&fatx_inode_lock);
	list_for_each(walk, p) {
		i = list_entry(walk, struct fatx_inode_info, i_fat_hash);
		if (i->i_fat_inode->i_sb != sb)
			continue;
		if (i->i_pos != i_pos)
			continue;
		inode = igrab(i->i_fat_inode);
		if (inode)
			break;
	}
	spin_unlock(&fatx_inode_lock);
	return inode;
}

/* doesn't deal with root inode */
static void fatx_fill_inode(struct inode *inode, struct fatx_dir_entry *de)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	int nr;

	INIT_LIST_HEAD(&FATX_I(inode)->i_fat_hash);
	FATX_I(inode)->i_pos = 0;
	FATX_I(inode)->i_fat_inode = inode;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = get_seconds();
	
	if ((de->attr & ATTR_DIR) && !FATX_IS_FREE(de)) {
		inode->i_generation &= ~1;
		inode->i_mode = FATX_MKMODE(de->attr,S_IRWXUGO & 
			~sbi->options.fs_umask) | S_IFDIR;
		inode->i_op = sbi->dir_ops;
		inode->i_fop = &fatx_dir_operations;

		FATX_I(inode)->i_start = CF_LE_L(de->start);
		FATX_I(inode)->i_logstart = FATX_I(inode)->i_start;
		inode->i_nlink = fatx_subdirs(inode) + 2;
		    /* includes .., compensating for "self" */
#ifdef DEBUG
		if (!inode->i_nlink) {
			printk("directory %d: i_nlink == 0\n",inode->i_ino);
			inode->i_nlink = 1;
		}
#endif
		if ((nr = FATX_I(inode)->i_start) != 0)
			while (nr != -1) {
				inode->i_size += 1 << sbi->cluster_bits;
				if (!(nr = fatx_access(sb, nr, -1))) {
					printk("Directory %ld: bad FAT\n",
					    inode->i_ino);
					break;
				}
			}
		FATX_I(inode)->mmu_private = inode->i_size;
	} else { /* not a directory */
		inode->i_generation |= 1;
		inode->i_mode = FATX_MKMODE(de->attr,S_IRWXUGO & ~sbi->options.fs_umask) | S_IFREG;
		FATX_I(inode)->i_start = CF_LE_L(de->start);
		FATX_I(inode)->i_logstart = FATX_I(inode)->i_start;
		inode->i_size = CF_LE_L(de->size);
	        inode->i_op = &fatx_file_inode_operations;
	        inode->i_fop = &fatx_file_operations;
		inode->i_mapping->a_ops = &fatx_aops;
		FATX_I(inode)->mmu_private = inode->i_size;
	}
	FATX_I(inode)->i_attrs = de->attr & ATTR_UNUSED;
	/* this is as close to the truth as we can get ... */
	inode->i_blksize = 1 << sbi->cluster_bits;
	inode->i_blocks = ((inode->i_size + inode->i_blksize - 1)
			   & ~(inode->i_blksize - 1)) >> 9;
	inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec =
		fatx_date_dos2unix(CF_LE_W(de->time),CF_LE_W(de->date));
	inode->i_ctime.tv_nsec = fatx_date_dos2unix(CF_LE_W(de->ctime),CF_LE_W(de->cdate));
}

struct inode *fatx_build_inode(	struct super_block *sb,	struct fatx_dir_entry *de, loff_t i_pos, int *res )
{
	struct inode *inode;
	*res = 0;
	inode = fatx_iget(sb, i_pos);
	if (inode)
		goto out;
	inode = new_inode(sb);
	*res = -ENOMEM;
	if (!inode)
		goto out;
	*res = 0;
	inode->i_ino = iunique(sb, FATX_ROOT_INO);
	fatx_fill_inode(inode, de);
	fatx_attach(inode, i_pos);
	insert_inode_hash(inode);
out:
	return inode;
}

/*
 * parse super block values out of FATX "boot block"
 * unlike the other FAT variants, much of the data is calculated from the
 * the partition information.
 */
int fatx_parse_boot_block ( struct super_block *sb, struct buffer_head *bh )
{
	struct fatx_boot_sector *b = (struct fatx_boot_sector *)bh->b_data;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	int logical_sector_size, hard_blksize;
	unsigned int total_sectors;
	unsigned long cl_count;
	unsigned long fat_length;

	PRINTK("FATX: entered fatx_parse_boot_block\n");
	
	if (b->magic != FATX_BOOTBLOCK_MAGIC) {
		printk("FATX: boot block signature not found.  Not FATX?\n");
		return -1;
	}
		
	PRINTK("FATX: fatx_magic: %08lX\n",(unsigned long)b->magic);
			
	logical_sector_size = 512;
	
	sbi->cluster_size = CLUSTER_SIZE;
	
	PRINTK("FATX: cluster_size: %d\n",(int)sbi->cluster_size);
	
	//sb->s_block_size enters as hardware block (sector) size
	hard_blksize = sb->s_blocksize;
	sb->s_blocksize = logical_sector_size;
	sb->s_blocksize_bits = ffs(logical_sector_size) - 1;

	//figure total sector count
	//total_sectors = fatx_get_total_size(sb);
	total_sectors = sb->s_bdev->bd_inode->i_size >> 9;
	
	PRINTK("FATX: total_sectors for given device: %ld\n",(unsigned long)total_sectors);
	
	sbi->cluster_bits = 14;
	sbi->fats = 1;
	
	//hmm...fat should start right after boot block sectors (first 8)
	sbi->fat_start = 8;	//this might be: + CF_LE_W(b->fatx_unknown)
	sbi->root_cluster = 0;
	sbi->dir_per_block = logical_sector_size/sizeof(struct fatx_dir_entry);
	sbi->dir_per_block_bits = ffs(sbi->dir_per_block) - 1;
	sbi->dir_entries = 256;
	
	//check cluster count
	
	cl_count = total_sectors / sbi->cluster_size;

	if( cl_count >= 0xfff4 ) {
		//FATX-32
		sb->s_maxbytes = FATX32_MAX_NON_LFS;
		sbi->fat_bits = 32;
	} else {
		//FATX-16
		sb->s_maxbytes = FATX16_MAX_NON_LFS;
		sbi->fat_bits = 16;
	}

	fat_length = cl_count * (sbi->fat_bits>>3);		
	if(fat_length % 4096) {
		fat_length = ((fat_length / 4096) + 1) * 4096;
	}
	sbi->fat_length = fat_length / logical_sector_size;

	sbi->dir_start = sbi->fat_start + sbi->fat_length;
	sbi->data_start = sbi->dir_start + CLUSTER_SIZE;
	sbi->clusters = ((total_sectors-sbi->data_start) / sbi->cluster_size);
	sbi->free_clusters = -1; /* Don't know yet */
	
	PRINTK("FATX: logical_sector_size:	%d\n",(int)logical_sector_size);
	PRINTK("FATX: fat_length:		%d\n",(int)sbi->fat_length);
	PRINTK("FATX: spc_bits:			%d\n",sbi->fat_bits>>3);
	PRINTK("FATX: fat_start:		%d\n",(int)sbi->fat_start);
	PRINTK("FATX: dir_start:		%d\n",(int)sbi->dir_start);
	PRINTK("FATX: data_start:		%d\n",(int)sbi->data_start);
	PRINTK("FATX: clusters:			%ld\n",(unsigned long)sbi->clusters);
	PRINTK("FATX: fat_bits:			%d\n",(int)sbi->fat_bits);
	PRINTK("FATX: fat_length:		%d\n",(int)sbi->fat_length);
	PRINTK("FATX: root_dir_sectors:		%d\n",(int)CLUSTER_SIZE);
	PRINTK("FATX: dir_per_block:		%d\n",(int)sbi->dir_per_block);
	PRINTK("FATX: dir_per_block_bits:	%d\n",(int)sbi->dir_per_block_bits);
	PRINTK("FATX: dir_entries :		%d\n",(int)sbi->dir_entries);
	PRINTK("FATX: cluster_bits:		%d\n",(int)sbi->cluster_bits);
	
	PRINTK("FATX: leaving fatx_parse_boot_block\n");
		
	return 0;
}

static void fatx_read_root(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);

	INIT_LIST_HEAD(&FATX_I(inode)->i_fat_hash);
	FATX_I(inode)->i_pos = 0;
	FATX_I(inode)->i_fat_inode = inode;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version = 1;
	inode->i_generation = 0;
	inode->i_mode = (S_IRWXUGO & ~sbi->options.fs_umask) | S_IFDIR;
	inode->i_op = sbi->dir_ops;
	inode->i_fop = &fatx_dir_operations;
	
	FATX_I(inode)->i_start = FATX_ROOT_INO;
	inode->i_size = sbi->dir_entries * sizeof(struct fatx_dir_entry);

	inode->i_blksize = 1 << sbi->cluster_bits;
	inode->i_blocks = ((inode->i_size + inode->i_blksize - 1)
			   & ~(inode->i_blksize - 1)) >> 9;
	FATX_I(inode)->i_logstart = 0;
	FATX_I(inode)->mmu_private = inode->i_size;

	FATX_I(inode)->i_attrs = 0;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec = 0;
	inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	FATX_I(inode)->i_ctime_ms = 0;
	inode->i_nlink = fatx_subdirs(inode) + 2;
}

/* The public inode operations for the fatx fs */
struct inode_operations fatx_dir_inode_operations = {
	.create		= fatx_create,
	.lookup		= fatx_lookup,
	.unlink		= fatx_unlink,
	.mkdir		= fatx_mkdir,
	.rmdir		= fatx_rmdir,
	.rename		= fatx_rename,
	.setattr	= fatx_notify_change,
};

void fatx_write_inode(struct inode *inode, int wait)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct fatx_dir_entry *raw_entry;
	unsigned int i_pos;
	
	PRINTK("FATX: fatx_write_inode: entered\n");

retry:
	i_pos = FATX_I(inode)->i_pos;
	if (inode->i_ino == FATX_ROOT_INO || !i_pos) {
		return;
	}
	lock_kernel();
	if (!(bh = sb_bread(sb, i_pos >> FATX_SB(sb)->dir_per_block_bits))) {
		PRINTK("dev = %s, ino = %d\n", sb->s_id, i_pos);
		fatx_fs_panic(sb, "fatx_write_inode: unable to read i-node block");
		unlock_kernel();
		return;
	}
	spin_lock(&fatx_inode_lock);
	if (i_pos != FATX_I(inode)->i_pos) {
		spin_unlock(&fatx_inode_lock);
		if(bh) brelse(bh);
		unlock_kernel();
		goto retry;
	}

	raw_entry = &((struct fatx_dir_entry *) (bh->b_data))
	    [i_pos & (FATX_SB(sb)->dir_per_block - 1)];
	if (S_ISDIR(inode->i_mode)) {
		raw_entry->attr = ATTR_DIR;
		raw_entry->size = 0;
	}
	else {
		raw_entry->attr = ATTR_NONE;
		raw_entry->size = CT_LE_L(inode->i_size);
	}
	raw_entry->attr |= FATX_MKATTR(inode->i_mode) |
	    FATX_I(inode)->i_attrs;
	raw_entry->start = CT_LE_L(FATX_I(inode)->i_logstart);
	
	PRINTK("FATX: fatx_write_inode: start == %08lX (LE=%08lX)\n",
			(long)FATX_I(inode)->i_logstart,
			(long)CT_LE_L(FATX_I(inode)->i_logstart));
	
	fatx_date_unix2dos(inode->i_mtime.tv_sec,&raw_entry->time,&raw_entry->date);
	raw_entry->time = CT_LE_W(raw_entry->time);
	raw_entry->date = CT_LE_W(raw_entry->date);
	
	fatx_date_unix2dos(inode->i_ctime.tv_sec,&raw_entry->ctime,&raw_entry->cdate);
	raw_entry->ctime = CT_LE_W(raw_entry->ctime);
	raw_entry->cdate = CT_LE_W(raw_entry->cdate);
	raw_entry->atime = CT_LE_W(raw_entry->ctime);
	raw_entry->adate = CT_LE_W(raw_entry->cdate);
	
	spin_unlock(&fatx_inode_lock);
	mark_buffer_dirty(bh);
	if(bh) brelse(bh);
	unlock_kernel();
	
	PRINTK("FATX: fatx_write_inode: leaving\n");
}

int fatx_statfs(struct super_block *sb,struct kstatfs *buf)
{
	int free,nr;

	lock_fatx(sb);
        if (FATX_SB(sb)->free_clusters != -1)
		free = FATX_SB(sb)->free_clusters;
	else {
		free = 0;
		for (nr = 2; nr < FATX_SB(sb)->clusters+2; nr++)
			if (!fatx_access(sb,nr,-1)) free++;
		FATX_SB(sb)->free_clusters = free;
	}
	unlock_fatx(sb);
	buf->f_type = sb->s_magic;
	buf->f_bsize = 1 << FATX_SB(sb)->cluster_bits;
	buf->f_blocks = FATX_SB(sb)->clusters;
	buf->f_bfree = free;
	buf->f_bavail = free;
	buf->f_namelen = FATX_MAX_NAME_LENGTH;
	return 0;
}


void fatx_delete_inode(struct inode *inode)
{
	if (!is_bad_inode(inode)) {
		lock_kernel();
		inode->i_size = 0;
		fatx_truncate(inode);
		unlock_kernel();
	}
	clear_inode(inode);
}

void fatx_clear_inode(struct inode *inode)
{
	if (is_bad_inode(inode))
		return;
	lock_kernel();
	spin_lock(&fatx_inode_lock);
	fatx_cache_inval_inode(inode);
	list_del(&FATX_I(inode)->i_fat_hash);
	spin_unlock(&fatx_inode_lock);
	unlock_kernel();
}

static struct super_operations fatx_sops = { 
	.write_inode	= fatx_write_inode,
	.delete_inode	= fatx_delete_inode,
	.put_super	= fatx_put_super,
	.statfs		= fatx_statfs,
	.clear_inode	= fatx_clear_inode,
	.read_inode	= make_bad_inode,
};

enum {
	Opt_uid, Opt_gid, Opt_umask, Opt_quiet, Opt_err,
};

static match_table_t fat_tokens = {
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
        {Opt_umask, "umask=%o"},
	{Opt_quiet, "quiet"},
	{Opt_err, NULL}
};

static int parse_options(char *options,struct fatx_mount_options *opts)
{
        char *p;
        substring_t args[MAX_OPT_ARGS];
        int option;

	opts->fs_uid = current->uid;
        opts->fs_gid = current->gid;
        opts->fs_umask = current->fs->umask;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;
		token = match_token(p, fat_tokens, args);
		switch(token) {
			case Opt_quiet:
				opts->quiet = 1;
				break;
			case Opt_uid:
				if (match_int(&args[0], &option))
					return 0;
				opts->fs_uid = option;
				break;
			case Opt_gid:
				if (match_int(&args[0], &option))
					return 0;
				opts->fs_gid = option;
				break;
			case Opt_umask:
				if (match_octal(&args[0], &option))
					return 0;
				opts->fs_umask = option;
				break;
			default:
				printk(KERN_ERR "FATX: Unrecognized mount option \"%s\" "
					"or missing value\n", p);
				return 0;
		}
	}
	return 1;
}

int fatx_fill_super(struct super_block *sb,void *data, int silent)
{
	struct inode *root_inode;
	struct buffer_head *bh;
	struct fatx_sb_info *sbi;
	int hard_blksize;
	int error;

	PRINTK("FATX: entering fatx_fill_super\n");

	sbi = kmalloc(sizeof(struct fatx_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	memset(sbi, 0, sizeof(struct fatx_sb_info));
	sbi->private_data = NULL;

	sbi->dir_ops = &fatx_dir_inode_operations;

	sb->s_op = &fatx_sops;

	hard_blksize = 512;

	//store fat value parsed into fatx_bits...possibly overridden later
	sbi->fat_bits = 0;
	
	error = -EINVAL;
        if (!parse_options((char *) data, &(sbi->options)))
                goto out_fail;
	
	fatx_cache_init();

	error = -EIO;

	sb->s_blocksize = hard_blksize;
	sb_set_blocksize(sb, hard_blksize);
	bh = sb_bread(sb, 0);
	if (bh == NULL) {
		PRINTK("FATX: unable to read boot sector\n");
		goto out_fail;
	}

	// insert call(s) to superblock parsing
	error = fatx_parse_boot_block(sb,bh);
	brelse(bh);

	if (error)
		goto out_invalid;

	sb_set_blocksize(sb, sb->s_blocksize);

	sb->s_magic = FATX_BOOTBLOCK_MAGIC;
	/* set up enough so that it can read an inode */
	init_MUTEX(&sbi->fatx_lock);
	sbi->prev_free = 0;

	
	sbi->nls_io = NULL;
	if (! sbi->nls_io)
		sbi->nls_io = load_nls_default();
	
	root_inode = new_inode(sb);
	if (!root_inode)
		goto out_unload_nls;
	root_inode->i_ino = FATX_ROOT_INO;
	fatx_read_root(root_inode);
	insert_inode_hash(root_inode);
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root)
		goto out_no_root;

	PRINTK("FATX: leave fatx_fill_super\n");
	
	return 0;

out_no_root:
	PRINTK("FATX: get root inode failed\n");
	iput(root_inode);
out_unload_nls:
	unload_nls(sbi->nls_io);
	goto out_fail;
out_invalid:
	if (!silent) {
		PRINTK("VFS: Can't find a valid FAT filesystem on dev %s.\n",
			sb->s_id);
	}
out_fail:
	if(sbi->private_data)
		kfree(sbi->private_data);
	sbi->private_data = NULL;
	kfree(sbi); 
	return error;
}
		
int fatx_notify_change(struct dentry * dentry, struct iattr * attr)
{
	struct super_block *sb = dentry->d_sb;
	struct inode *inode = dentry->d_inode;
	int error;

	/* FAT cannot truncate to a longer file */
	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size > inode->i_size)
			return -EPERM;
	}

	error = inode_change_ok(inode, attr);
	if (error)
		return FATX_SB(sb)->options.quiet ? 0 : error;

	if (((attr->ia_valid & ATTR_UID) && 
	     (attr->ia_uid != FATX_SB(sb)->options.fs_uid)) ||
	    ((attr->ia_valid & ATTR_GID) && 
	     (attr->ia_gid != FATX_SB(sb)->options.fs_gid)) ||
	    ((attr->ia_valid & ATTR_MODE) &&
	     (attr->ia_mode & ~FATX_VALID_MODE)))
		error = -EPERM;

	if (error)
		return FATX_SB(sb)->options.quiet ? 0 : error;

	error = inode_setattr(inode, attr);
	if (error)
		return error;

	if (S_ISDIR(inode->i_mode))
		inode->i_mode |= S_IXUGO;

	inode->i_mode = ((inode->i_mode & S_IFMT) | ((((inode->i_mode & S_IRWXU
	    & ~FATX_SB(sb)->options.fs_umask) | S_IRUSR) >> 6)*S_IXUGO)) &
	    ~FATX_SB(sb)->options.fs_umask;
	return 0;
}

