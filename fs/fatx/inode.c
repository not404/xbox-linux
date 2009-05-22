/*
 *  linux/fs/fatx/inode.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  VFAT extensions by Gordon Chaffee, merged with fatx fs by Henrik Storner
 *  Rewritten for the constant inumbers support by Al Viro
 *
 *  Fixes:
 *
 *	Max Cohan: Fixed invalid FSINFO offset when info_sector is 0
 *
 *  FATX port 2005 by Edgar Hucek
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/seq_file.h>
#include <linux/fatx_fs.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <asm/unaligned.h>

#ifndef CONFIG_FAT_DEFAULT_IOCHARSET
/* if user don't select VFAT, this is undefined. */
#define CONFIG_FAT_DEFAULT_IOCHARSET	""
#endif

unsigned int fatx_debug = 0;

#define PRINTK(format, args...) do { if (fatx_debug) printk( format, ##args ); } while(0)



static __s64 fatx_add_cluster(struct inode *inode)
{
	__s64 err, cluster;

	PRINTK("FATX: %s\n", __FUNCTION__);
	err = fatx_alloc_clusters(inode, &cluster, 1);
	if (err) {
		PRINTK("FATX: %s fatx_alloc_clusters error cluster 0x%08llx\n", __FUNCTION__, cluster);
		return err;
	}
	/* FIXME: this cluster should be added after data of this
	 * cluster is writed */
	err = fatx_chain_add(inode, cluster, 1);
	if (err) {
		PRINTK("FATX: %s fatx_chain_add error cluster 0x%08llx\n", __FUNCTION__, cluster);
		fatx_free_clusters(inode, cluster);
	}
	return err;
}

static int fatx_get_block(struct inode *inode, sector_t iblock,
			 struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	sector_t phys;
	__s64 err;

	err = fatx_bmap(inode, iblock, &phys);
	if (err)
		return err;
	if (phys) {
		map_bh(bh_result, sb, phys);
		return 0;
	}
	if (!create)
		return 0;
	if (iblock != FATX_I(inode)->mmu_private >> sb->s_blocksize_bits) {
		fatx_fs_panic(sb, "corrupted file size (i_pos %lld, %lld)",
			     FATX_I(inode)->i_pos, FATX_I(inode)->mmu_private);
		return -EIO;
	}
	if (!((unsigned long)iblock & (FATX_SB(sb)->sec_per_clus - 1))) {
		PRINTK("FATX: fatx_get_block -> fatx_add_cluster 0x%08lx\n",
			(unsigned long)iblock);
		err = fatx_add_cluster(inode);
		if (err)
			return err;
	}
	FATX_I(inode)->mmu_private += sb->s_blocksize;
	err = fatx_bmap(inode, iblock, &phys);
	if (err)
		return err;
	if (!phys)
		BUG();
	set_buffer_new(bh_result);
	map_bh(bh_result, sb, phys);
	return 0;
}

static int fatx_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, fatx_get_block, wbc);
}

static int fatx_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, fatx_get_block);
}

static int fatx_prepare_write(struct file *file, struct page *page,
			     unsigned from, unsigned to)
{
	return cont_prepare_write(page, from, to, fatx_get_block,
				  &FATX_I(page->mapping->host)->mmu_private);
}

static sector_t _fatx_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, fatx_get_block);
}

static struct address_space_operations fatx_aops = {
	.readpage	= fatx_readpage,
	.writepage	= fatx_writepage,
	.sync_page	= block_sync_page,
	.prepare_write	= fatx_prepare_write,
	.commit_write	= generic_commit_write,
	.bmap		= _fatx_bmap
};

/*
 * New FAT inode stuff. We do the following:
 *	a) i_ino is constant and has nothing with on-disk location.
 *	b) FAT manages its own cache of directory entries.
 *	c) *This* cache is indexed by on-disk location.
 *	d) inode has an associated directory entry, all right, but
 *		it may be unhashed.
 *	e) currently entries are stored within struct inode. That should
 *		change.
 *	f) we deal with races in the following way:
 *		1. readdir() and lookup() do FAT-dir-cache lookup.
 *		2. rename() unhashes the F-d-c entry and rehashes it in
 *			a new place.
 *		3. unlink() and rmdir() unhash F-d-c entry.
 *		4. fatx_write_inode() checks whether the thing is unhashed.
 *			If it is we silently return. If it isn't we do bread(),
 *			check if the location is still valid and retry if it
 *			isn't. Otherwise we do changes.
 *		5. Spinlock is used to protect hash/unhash/location check/lookup
 *		6. fatx_clear_inode() unhashes the F-d-c entry.
 *		7. lookup() and readdir() do igrab() if they find a F-d-c entry
 *			and consider negative result as cache miss.
 */

static void fatx_hash_init(struct super_block *sb)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	int i;

	spin_lock_init(&sbi->inode_hash_lock);
	for (i = 0; i < FAT_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&sbi->inode_hashtable[i]);
}

static inline unsigned long fatx_hash(struct super_block *sb, loff_t i_pos)
{
	unsigned long tmp = (unsigned long)i_pos | (unsigned long) sb;
	tmp = tmp + (tmp >> FAT_HASH_BITS) + (tmp >> FAT_HASH_BITS * 2);
	return tmp & FAT_HASH_MASK;
}

void fatx_attach(struct inode *inode, loff_t i_pos)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);

	spin_lock(&sbi->inode_hash_lock);
	FATX_I(inode)->i_pos = i_pos;
	hlist_add_head(&FATX_I(inode)->i_fatx_hash,
			sbi->inode_hashtable + fatx_hash(sb, i_pos));
	spin_unlock(&sbi->inode_hash_lock);
}

EXPORT_SYMBOL(fatx_attach);

void fatx_detach(struct inode *inode)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	spin_lock(&sbi->inode_hash_lock);
	FATX_I(inode)->i_pos = 0;
	hlist_del_init(&FATX_I(inode)->i_fatx_hash);
	spin_unlock(&sbi->inode_hash_lock);
}

EXPORT_SYMBOL(fatx_detach);

struct inode *fatx_iget(struct super_block *sb, loff_t i_pos)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct hlist_head *head = sbi->inode_hashtable + fatx_hash(sb, i_pos);
	struct hlist_node *_p;
	struct fatx_inode_info *i;
	struct inode *inode = NULL;

	spin_lock(&sbi->inode_hash_lock);
	hlist_for_each_entry(i, _p, head, i_fatx_hash) {
		BUG_ON(i->vfs_inode.i_sb != sb);
		if (i->i_pos != i_pos)
			continue;
		inode = igrab(&i->vfs_inode);
		if (inode)
			break;
	}
	spin_unlock(&sbi->inode_hash_lock);
	return inode;
}

static int fatx_calc_dir_size(struct inode *inode)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	__s64 ret; 
	int fclus, dclus;

	inode->i_size = 0;
	if (FATX_I(inode)->i_start == 0)
		return 0;

	ret = fatx_get_cluster(inode, FAT_ENT_EOF, &fclus, &dclus);
	if (ret < 0)
		return ret;
	inode->i_size = (fclus + 1) << sbi->cluster_bits;

	return 0;
}

/* doesn't deal with root inode */
static int fatx_fill_inode(struct inode *inode, struct fatx_dir_entry *de)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);
	int error;

	FATX_I(inode)->i_pos = 0;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = get_seconds();

	if ((de->attr & ATTR_DIR) && !IS_FREE(de)) {
		inode->i_generation &= ~1;
		inode->i_mode = FATX_MKMODE(de->attr, S_IRWXUGO & ~sbi->options.fs_dmask) | S_IFDIR;
		inode->i_op = sbi->dir_ops;
		inode->i_fop = &fatx_dir_operations;

		FATX_I(inode)->i_start = le32_to_cpu(de->start);
		FATX_I(inode)->i_logstart = FATX_I(inode)->i_start;
		error = fatx_calc_dir_size(inode);
		if (error < 0) {
			PRINTK("FATX: %s fatx_calc_dir_size failed\n", __FUNCTION__ );
			return error;
		}
		FATX_I(inode)->mmu_private = inode->i_size;

		// TODO: check if 2 is right
		inode->i_nlink = fatx_subdirs(inode) + 2;
	} else { /* not a directory */
		inode->i_generation |= 1;
		inode->i_mode = FATX_MKMODE(de->attr,S_IRWXUGO & ~sbi->options.fs_fmask) | S_IFREG;
		FATX_I(inode)->i_start = le32_to_cpu(de->start);
		FATX_I(inode)->i_logstart = FATX_I(inode)->i_start;
		inode->i_size = le32_to_cpu(de->size);
		inode->i_op = &fatx_file_inode_operations;
		inode->i_fop = &fatx_file_operations;
		inode->i_mapping->a_ops = &fatx_aops;
		FATX_I(inode)->mmu_private = inode->i_size;
	}
	FATX_I(inode)->i_attrs = de->attr & ATTR_UNUSED;
	/* this is as close to the truth as we can get ... */
	inode->i_blksize = sbi->cluster_size;
	inode->i_blocks = ((inode->i_size + (sbi->cluster_size - 1))
			   & ~((loff_t)sbi->cluster_size - 1)) >> 9;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec =
		fatx_date_dos2unix(le16_to_cpu(de->time), le16_to_cpu(de->date));
	inode->i_ctime = inode->i_mtime;
	return 0;
}

struct inode *fatx_build_inode(struct super_block *sb,
			struct fatx_dir_entry *de, loff_t i_pos)
{
	struct inode *inode;
	int err;

	inode = fatx_iget(sb, i_pos);
	if (inode) {
		PRINTK("FATX: %s fatx_iget failed\n", __FUNCTION__ );
		goto out;
	}
	inode = new_inode(sb);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}
	inode->i_ino = iunique(sb, FATX_ROOT_INO);
	inode->i_version = 1;
	err = fatx_fill_inode(inode, de);
	if (err) {
		iput(inode);
		inode = ERR_PTR(err);
		PRINTK("FATX: %s fatx_fill_inode failed\n", __FUNCTION__ );
		goto out;
	}
	fatx_attach(inode, i_pos);
	insert_inode_hash(inode);
out:
	return inode;
}

EXPORT_SYMBOL(fatx_build_inode);

static void fatx_delete_inode(struct inode *inode)
{
	if (!is_bad_inode(inode)) {
		inode->i_size = 0;
		fatx_truncate(inode);
	}
	clear_inode(inode);
}

static void fatx_clear_inode(struct inode *inode)
{
	struct fatx_sb_info *sbi = FATX_SB(inode->i_sb);

	if (is_bad_inode(inode))
		return;
	lock_kernel();
	spin_lock(&sbi->inode_hash_lock);
	fatx_cache_inval_inode(inode);
	hlist_del_init(&FATX_I(inode)->i_fatx_hash);
	spin_unlock(&sbi->inode_hash_lock);
	unlock_kernel();
}

static void fatx_put_super(struct super_block *sb)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);

	if (sbi->nls_io) {
		unload_nls(sbi->nls_io);
		sbi->nls_io = NULL;
	}

	sb->s_fs_info = NULL;
	kfree(sbi);
}

static kmem_cache_t *fatx_inode_cachep;

static struct inode *fatx_alloc_inode(struct super_block *sb)
{
	struct fatx_inode_info *ei;
	ei = kmem_cache_alloc(fatx_inode_cachep, SLAB_KERNEL);
	if (!ei)
		return NULL;
	return &ei->vfs_inode;
}

static void fatx_destroy_inode(struct inode *inode)
{
	kmem_cache_free(fatx_inode_cachep, FATX_I(inode));
}

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct fatx_inode_info *ei = (struct fatx_inode_info *)foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		spin_lock_init(&ei->cache_lru_lock);
		ei->nr_caches = 0;
		ei->cache_valid_id = FAT_CACHE_VALID + 1;
		INIT_LIST_HEAD(&ei->cache_lru);
		INIT_HLIST_NODE(&ei->i_fatx_hash);
		inode_init_once(&ei->vfs_inode);
	}
}

static int __init fatx_init_inodecache(void)
{
	fatx_inode_cachep = kmem_cache_create("fatx_inode_cache",
					     sizeof(struct fatx_inode_info),
					     0, SLAB_RECLAIM_ACCOUNT,
					     init_once, NULL);
	if (fatx_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void __exit fatx_destroy_inodecache(void)
{
	if (kmem_cache_destroy(fatx_inode_cachep))
		printk(KERN_INFO "fatx_inode_cache: not all structures were freed\n");
}

static int fatx_remount(struct super_block *sb, int *flags, char *data)
{
	*flags |= MS_NODIRATIME | MS_NOATIME;
	return 0;
}

static int fatx_statfs(struct super_block *sb, struct kstatfs *buf)
{
	struct fatx_sb_info *sbi = FATX_SB(sb);

	/* If the count of free cluster is still unknown, counts it here. */
	if (sbi->free_clusters == -1) {
		__s64 err = fatx_count_free_clusters(sb);
		if (err)
			return err;
	}

	buf->f_type = sb->s_magic;
	buf->f_bsize = sbi->cluster_size;
	buf->f_blocks = sbi->max_cluster - FAT_START_ENT;
	buf->f_bfree = sbi->free_clusters;
	buf->f_bavail = sbi->free_clusters;
	buf->f_namelen = FATX_NAME;

	return 0;
}

static int fatx_write_inode(struct inode *inode, int wait)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	struct buffer_head *bh;
	struct fatx_dir_entry *raw_entry;
	loff_t i_pos;
	int err = 0;

retry:
	i_pos = FATX_I(inode)->i_pos;
	if (inode->i_ino == FATX_ROOT_INO || !i_pos)
		return 0;

	lock_kernel();
	bh = sb_bread(sb, i_pos >> sbi->dir_per_block_bits);
	if (!bh) {
		printk(KERN_ERR "FATX: unable to read inode block "
		       "for updating (i_pos %lld)\n", i_pos);
		err = -EIO;
		goto out;
	}
	spin_lock(&sbi->inode_hash_lock);
	if (i_pos != FATX_I(inode)->i_pos) {
		spin_unlock(&sbi->inode_hash_lock);
		brelse(bh);
		unlock_kernel();
		goto retry;
	}

	raw_entry = &((struct fatx_dir_entry *) (bh->b_data))
	    [i_pos & (sbi->dir_per_block - 1)];
	if (S_ISDIR(inode->i_mode))
		raw_entry->size = 0;
	else
		raw_entry->size = cpu_to_le32(inode->i_size);
	raw_entry->attr = fatx_attr(inode);
	raw_entry->start = cpu_to_le32(FATX_I(inode)->i_logstart);
	
	fatx_date_unix2dos(inode->i_mtime.tv_sec, &raw_entry->time, &raw_entry->date);
	//raw_entry->time = cpu_to_le16(raw_entry->time);
	//raw_entry->date = cpu_to_le16(raw_entry->date);
	
	//fatx_date_unix2dos(inode->i_mtime.tv_sec, &raw_entry->time, &raw_entry->date);	
	//raw_entry->ctime = cpu_to_le16(raw_entry->ctime);
	//raw_entry->cdate = cpu_to_le16(raw_entry->cdate);
	//raw_entry->atime = cpu_to_le16(raw_entry->ctime);
	//raw_entry->adate = cpu_to_le16(raw_entry->cdate);
	
	spin_unlock(&sbi->inode_hash_lock);
	mark_buffer_dirty(bh);
	if (wait)
		err = sync_dirty_buffer(bh);
	brelse(bh);
out:
	unlock_kernel();
	return err;
}

int fatx_sync_inode(struct inode *inode)
{
	return fatx_write_inode(inode, 1);
}

EXPORT_SYMBOL(fatx_sync_inode);

static int fatx_show_options(struct seq_file *m, struct vfsmount *mnt);
static struct super_operations fatx_sops = {
	.alloc_inode	= fatx_alloc_inode,
	.destroy_inode	= fatx_destroy_inode,
	.write_inode	= fatx_write_inode,
	.delete_inode	= fatx_delete_inode,
	.put_super	= fatx_put_super,
	.statfs		= fatx_statfs,
	.clear_inode	= fatx_clear_inode,
	.remount_fs	= fatx_remount,

	.read_inode	= make_bad_inode,

	.show_options	= fatx_show_options,
};

/*
 * a FAT file handle with fhtype 3 is
 *  0/  i_ino - for fast, reliable lookup if still in the cache
 *  1/  i_generation - to see if i_ino is still valid
 *          bit 0 == 0 iff directory
 *  2/  i_pos(8-39) - if ino has changed, but still in cache
 *  3/  i_pos(4-7)|i_logstart - to semi-verify inode found at i_pos
 *  4/  i_pos(0-3)|parent->i_logstart - maybe used to hunt for the file on disc
 *
 * Hack for NFSv2: Maximum FAT entry number is 28bits and maximum
 * i_pos is 40bits (blocknr(32) + dir offset(8)), so two 4bits
 * of i_logstart is used to store the directory entry offset.
 */

static struct dentry *
fatx_decode_fh(struct super_block *sb, __u32 *fh, int len, int fhtype,
	      int (*acceptable)(void *context, struct dentry *de),
	      void *context)
{
	if (fhtype != 3)
		return ERR_PTR(-ESTALE);
	if (len < 5)
		return ERR_PTR(-ESTALE);

	return sb->s_export_op->find_exported_dentry(sb, fh, NULL, acceptable, context);
}

static struct dentry *fatx_get_dentry(struct super_block *sb, void *inump)
{
	struct inode *inode = NULL;
	struct dentry *result;
	__u32 *fh = inump;

	inode = iget(sb, fh[0]);
	if (!inode || is_bad_inode(inode) || inode->i_generation != fh[1]) {
		if (inode)
			iput(inode);
		inode = NULL;
	}
	if (!inode) {
		loff_t i_pos;
		int i_logstart = fh[3];

		i_pos = (loff_t)fh[2] << 8;
		i_pos |= ((fh[3] >> 24) & 0xf0) | (fh[4] >> 28);

		/* try 2 - see if i_pos is in F-d-c
		 * require i_logstart to be the same
		 * Will fail if you truncate and then re-write
		 */

		inode = fatx_iget(sb, i_pos);
		if (inode && FATX_I(inode)->i_logstart != i_logstart) {
			iput(inode);
			inode = NULL;
		}
	}
	if (!inode) {
		/* For now, do nothing
		 * What we could do is:
		 * follow the file starting at fh[4], and record
		 * the ".." entry, and the name of the fh[2] entry.
		 * The follow the ".." file finding the next step up.
		 * This way we build a path to the root of
		 * the tree. If this works, we lookup the path and so
		 * get this inode into the cache.
		 * Finally try the fatx_iget lookup again
		 * If that fails, then weare totally out of luck
		 * But all that is for another day
		 */
	}
	if (!inode)
		return ERR_PTR(-ESTALE);


	/* now to find a dentry.
	 * If possible, get a well-connected one
	 */
	result = d_alloc_anon(inode);
	if (result == NULL) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	result->d_op = sb->s_root->d_op;
	return result;
}

static int
fatx_encode_fh(struct dentry *de, __u32 *fh, int *lenp, int connectable)
{
	int len = *lenp;
	struct inode *inode =  de->d_inode;
	u32 ipos_h, ipos_m, ipos_l;

	if (len < 5)
		return 255; /* no room */

	ipos_h = FATX_I(inode)->i_pos >> 8;
	ipos_m = (FATX_I(inode)->i_pos & 0xf0) << 24;
	ipos_l = (FATX_I(inode)->i_pos & 0x0f) << 28;
	*lenp = 5;
	fh[0] = inode->i_ino;
	fh[1] = inode->i_generation;
	fh[2] = ipos_h;
	fh[3] = ipos_m | FATX_I(inode)->i_logstart;
	spin_lock(&de->d_lock);
	fh[4] = ipos_l | FATX_I(de->d_parent->d_inode)->i_logstart;
	spin_unlock(&de->d_lock);
	return 3;
}

static struct dentry *fatx_get_parent(struct dentry *child)
{
	struct buffer_head *bh = NULL;
	struct fatx_dir_entry *de = NULL;
	loff_t i_pos = 0;
	struct dentry *parent;
	struct inode *inode;
	/*
	int err;
	*/

	lock_kernel();

	/*
	err = fatx_get_dotdot_entry(child->d_inode, &bh, &de, &i_pos);
	if(err) {
		parent = ERR_PTR(err);
		goto out;
	}
	*/

	inode = fatx_build_inode(child->d_sb, de, i_pos);
	brelse(bh);
	if (IS_ERR(inode)) {
		parent = ERR_PTR(PTR_ERR(inode));
		goto out;
	}
	parent = d_alloc_anon(inode);
	if (!parent) {
		iput(inode);
		parent = ERR_PTR(-ENOMEM);
	}
out:
	unlock_kernel();

	return parent;
}

static struct export_operations fatx_export_ops = {
	.decode_fh	= fatx_decode_fh,
	.encode_fh	= fatx_encode_fh,
	.get_dentry	= fatx_get_dentry,
	.get_parent	= fatx_get_parent,
};

static int fatx_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	struct fatx_sb_info *sbi = FATX_SB(mnt->mnt_sb);
	struct fatx_mount_options *opts = &sbi->options;

	if (opts->fs_uid != 0)
		seq_printf(m, ",uid=%u", opts->fs_uid);
	if (opts->fs_gid != 0)
		seq_printf(m, ",gid=%u", opts->fs_gid);
	seq_printf(m, ",fmask=%04o", opts->fs_fmask);
	seq_printf(m, ",dmask=%04o", opts->fs_dmask);
	if (opts->quiet)
		seq_puts(m, ",quiet");
	return 0;
}

enum {
	Opt_uid, Opt_gid, Opt_umask, Opt_dmask, Opt_fmask, 
	Opt_quiet, Opt_err,
};

static match_table_t fatx_tokens = {
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_umask, "umask=%o"},
	{Opt_dmask, "dmask=%o"},
	{Opt_fmask, "fmask=%o"},
	{Opt_err, NULL}
};

static int parse_options(char *options, struct fatx_mount_options *opts)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;

	opts->fs_uid = current->uid;
	opts->fs_gid = current->gid;
	opts->fs_fmask = opts->fs_dmask = current->fs->umask;
	opts->quiet = 0;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, fatx_tokens, args);
		if (token == Opt_err) {
			token = match_token(p, fatx_tokens, args);
		}
		switch (token) {
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
			opts->fs_fmask = opts->fs_dmask = option;
			break;
		case Opt_dmask:
			if (match_octal(&args[0], &option))
				return 0;
			opts->fs_dmask = option;
			break;
		case Opt_fmask:
			if (match_octal(&args[0], &option))
				return 0;
			opts->fs_fmask = option;
			break;
		default:
			printk(KERN_ERR "FATX: Unrecognized mount option \"%s\" "
			       "or missing value\n", p);
			return -EINVAL;
		}
	}
	return 0;
}

static int fatx_read_root(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct fatx_sb_info *sbi = FATX_SB(sb);
	int error;

	FATX_I(inode)->i_pos = 0;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = 0;
	inode->i_mode = (S_IRWXUGO & ~sbi->options.fs_dmask) | S_IFDIR;
	inode->i_op = sbi->dir_ops;
	inode->i_fop = &fatx_dir_operations;
	FATX_I(inode)->i_start = 0;
	error = fatx_calc_dir_size(inode);
	if (error < 0)
		return error;
	inode->i_size = sbi->dir_entries * sizeof(struct fatx_dir_entry);
	inode->i_blksize = sbi->cluster_size;
	inode->i_blocks = ((inode->i_size + (sbi->cluster_size - 1))
			   & ~((loff_t)sbi->cluster_size - 1)) >> 9;
	FATX_I(inode)->i_logstart = 0;
	FATX_I(inode)->mmu_private = inode->i_size;

	FATX_I(inode)->i_attrs = ATTR_NONE;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec = 0;
	inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec = inode->i_ctime.tv_nsec = 1;
	//FATX_I(inode)->i_ctime_ms = 0;
	inode->i_nlink = fatx_subdirs(inode)+2;

	return 0;
}

/*
 * Read the super block of an MS-DOS FS.
 */
int fatx_fill_super_inode(struct super_block *sb, void *data, int silent,
		   struct inode_operations *fs_dir_inode_ops)
{
	struct inode *root_inode = NULL;
	struct buffer_head *bh;
	struct fatx_boot_sector *b;
	struct fatx_sb_info *sbi;
	u16 logical_sector_size;
	//u32 total_clusters;
	u32 total_sectors;
	unsigned long cl_count;
	unsigned long fatx_length;
	long error;

	sbi = kmalloc(sizeof(struct fatx_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	memset(sbi, 0, sizeof(struct fatx_sb_info));

	sb->s_flags |= MS_NODIRATIME;
	sb->s_magic = FATX_SUPER_MAGIC;
	sb->s_op = &fatx_sops;
	sb->s_export_op = &fatx_export_ops;
	sbi->dir_ops = fs_dir_inode_ops;

	error = parse_options(data, &sbi->options);
	if (error)
		goto out_fail;

	error = -EIO;
	sb_min_blocksize(sb, 512);
	bh = sb_bread(sb, 0);
	if (bh == NULL) {
		printk(KERN_ERR "FATX: unable to read boot sector\n");
		goto out_fail;
	}

	b = (struct fatx_boot_sector *) bh->b_data;

	logical_sector_size = 512;

	if (logical_sector_size < sb->s_blocksize) {
		printk(KERN_ERR "FATX: logical sector size too small for device"
		       " (logical sector size = %u)\n", logical_sector_size);
		brelse(bh);
		goto out_fail;
	}
	if (logical_sector_size > sb->s_blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, logical_sector_size)) {
			printk(KERN_ERR "FATX: unable to set blocksize %u\n",
			       logical_sector_size);
			goto out_fail;
		}
		bh = sb_bread(sb, 0);
		if (bh == NULL) {
			printk(KERN_ERR "FATX: unable to read boot sector"
			       " (logical sector size = %lu)\n",
			       sb->s_blocksize);
			goto out_fail;
		}
		b = (struct fatx_boot_sector *) bh->b_data;
	}

	sbi->sec_per_clus = CLUSTER_SIZE;
	sbi->cluster_size = sb->s_blocksize * sbi->sec_per_clus;
	sbi->cluster_bits = ffs(sbi->cluster_size) - 1;
	sbi->fatxs = 1;
	sbi->fatx_start = 8;
	sbi->root_cluster = 0;
	sbi->free_clusters = -1;	/* Don't know yet */
	sbi->prev_free = 0;

	total_sectors = sb->s_bdev->bd_inode->i_size >> 9;

	cl_count = total_sectors / sbi->sec_per_clus;
	if( cl_count >= 0xfff4 ) {
		//sb->s_maxbytes = FATX32_MAX_NON_LFS;
		sbi->fatx_bits = 32;
	} else {
		//sb->s_maxbytes = FATX16_MAX_NON_LFS;
		sbi->fatx_bits = 16;
	}
	sb->s_maxbytes = 0xffffffff;

	fatx_length = cl_count * (sbi->fatx_bits>>3);
	if(fatx_length % 4096) {
		fatx_length = ((fatx_length / 4096) + 1) * 4096;
	}
	sbi->fatx_length = fatx_length / logical_sector_size;
	
	sbi->dir_per_block = sb->s_blocksize / sizeof(struct fatx_dir_entry);
	sbi->dir_per_block_bits = ffs(sbi->dir_per_block) - 1;

	sbi->dir_start = sbi->fatx_start + sbi->fatx_length;
	sbi->dir_entries = 256;

	sbi->data_start = sbi->dir_start + sbi->sec_per_clus;

	sbi->max_cluster = ((total_sectors-sbi->data_start) / sbi->sec_per_clus) + FAT_START_ENT;

	sbi->nls_disk = NULL;
	brelse(bh);

	/* set up enough so that it can read an inode */
	fatx_hash_init(sb);
	fatx_ent_access_init(sb);

	error = -ENOMEM;
	root_inode = new_inode(sb);
	if (!root_inode)
		goto out_fail;
	root_inode->i_ino = FATX_ROOT_INO;
	root_inode->i_version = 1;
	error = fatx_read_root(root_inode);
	if (error < 0)
		goto out_fail;
	error = -ENOMEM;
	insert_inode_hash(root_inode);
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root) {
		printk(KERN_ERR "FATX: get root inode failed\n");
		goto out_fail;
	}

	PRINTK("FATX: logical_sector_size:	%d\n",(int)logical_sector_size);
	PRINTK("FATX: fatx_length:		%d\n",(int)sbi->fatx_length);
	PRINTK("FATX: spc_bits:			%d\n",sbi->fatx_bits>>3);
	PRINTK("FATX: fatx_start:		%d\n",(int)sbi->fatx_start);
	PRINTK("FATX: dir_start:		%d\n",(int)sbi->dir_start);
	PRINTK("FATX: data_start:		%d\n",(int)sbi->data_start);
	PRINTK("FATX: max_cluster:		%ld\n",(unsigned long)sbi->max_cluster);
	PRINTK("FATX: fatx_bits:		%d\n",(int)sbi->fatx_bits);
	PRINTK("FATX: fatx_length:		%d\n",(int)sbi->fatx_length);
	PRINTK("FATX: root_dir_sectors:		%d\n",(int)CLUSTER_SIZE);
	PRINTK("FATX: dir_per_block:		%d\n",(int)sbi->dir_per_block);
	PRINTK("FATX: dir_per_block_bits:	%d\n",(int)sbi->dir_per_block_bits);
	PRINTK("FATX: dir_entries :		%d\n",(int)sbi->dir_entries);
	PRINTK("FATX: cluster_bits:		%d\n",(int)sbi->cluster_bits);
	PRINTK("FATX: sec_per_clus:		%d\n",(int)sbi->sec_per_clus);
	PRINTK("FATX: cluster_size:		%d\n",(int)sbi->cluster_size);
	PRINTK("FATX: s_blocksize_bits:		%d\n",(int)sb->s_blocksize_bits);
	PRINTK("FATX: fatx_bits:		%d\n",(int)sbi->fatx_bits);
	PRINTK("FATX: leaving total_sectors:	%d\n",total_sectors);
	PRINTK("FATX: sizeof fatx_dir_entry:	%d\n",sizeof(struct fatx_dir_entry));

	return 0;

out_fail:
	if (root_inode)
		iput(root_inode);
	if (sbi->nls_io)
		unload_nls(sbi->nls_io);
	kfree(sbi);
	return error;
}

EXPORT_SYMBOL(fatx_fill_super_inode);

int __init fatx_cache_init(void);
void __exit fatx_cache_destroy(void);

static struct file_system_type fatx_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "fatx",
	.get_sb		= fatx_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_fatx_fs(void)
{
	int ret;

	ret = fatx_cache_init();
	if (ret < 0)
		return ret;
	printk("FATX: 0.0.3\n");
	fatx_init_inodecache();
	return register_filesystem(&fatx_fs_type);
}

static void __exit exit_fatx_fs(void)
{
	fatx_cache_destroy();
	fatx_destroy_inodecache();
	unregister_filesystem(&fatx_fs_type);
}

module_init(init_fatx_fs)
module_exit(exit_fatx_fs)

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FATX filesystem support");
MODULE_PARM(fatx_debug,"i");
MODULE_PARM_DESC(fatxx_debug,"turn on fatxx debugging output");
