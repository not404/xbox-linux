#ifndef _LINUX_FATX_FS_H
#define _LINUX_FATX_FS_H

/*
 *  The FATX filesystem constants/structures
 *
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <asm/byteorder.h>

#define FATX_ROOT_INO  1 /* == MINIX_ROOT_INO */

#define FATX_CACHE    8 /* FAT cache size */

#define MSDOS_DOTDOT "..         " /* "..", padded to MSDOS_NAME chars */

#define FATX_ENT_FREE   (0)
#define FATX32_MAX_NON_LFS     ((1UL<<32) - 1)
#define FATX16_MAX_NON_LFS     ((1UL<<30) - 1)
	
#define CLUSTER_SIZE 32
#define ATTR_RO      1  /* read-only */
#define ATTR_HIDDEN  2  /* hidden */
#define ATTR_SYS     4  /* system */
#define ATTR_DIR     16 /* directory */
#define ATTR_ARCH    0  /* archived */
#define ATTR_UNUSED  (ATTR_ARCH | ATTR_SYS | ATTR_HIDDEN)

#define ATTR_NONE    0 /* no attribute bits */

#define FATX_BOOTBLOCK_MAGIC cpu_to_le32(0x58544146)
#define FATX_MAX_NAME_LENGTH 42
#define FATX_DIR_BITS 6

#define DELETED_FLAG 0xe5 /* marks file as deleted when in name_length */

//FF == end of directory info, E5 == deleted entry
#define FATX_IS_FREE(de) ((de)->name_length==DELETED_FLAG)
#define FATX_END_OF_DIR(de) ((de)->name_length==0xFF)

#define IS_FREE(n) (!*(n) || *(const unsigned char *) (n) == DELETED_FLAG)

#define CF_LE_W(v) le16_to_cpu(v)
#define CF_LE_L(v) le32_to_cpu(v)
#define CT_LE_W(v) cpu_to_le16(v)
#define CT_LE_L(v) cpu_to_le32(v)

/* bad cluster mark */
#define BAD_FAT16 0xFFF7
#define BAD_FAT32 0x0FFFFFF7
#define BAD_FAT(s) (FATX_SB(s)->fat_bits == 32 ? BAD_FAT32 : BAD_FAT16)

#define EOC_FAT16 0xFFF8	// end of chain marker
#define EOC_FAT32 0xFFFFFFF8	// end of chain marker
#define EOF_FAT16 0xFFF8	// end of file marker
#define EOF_FAT32 0xFFFFFFF8	// end of file marker
#define EOF_FAT(s) (FATX_SB(s)->fat_bits == 32 ? EOF_FAT32 : EOF_FAT16)

#define FATX_VALID_MODE (S_IFREG | S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)

/* Convert attribute bits and a mask to the UNIX mode. */
#define FATX_MKMODE(a,m) (m & (a & ATTR_RO ? S_IRUGO|S_IXUGO : S_IRWXUGO))

/* Convert the UNIX mode to FATX attribute bits. */
#define FATX_MKATTR(m) ((m & S_IWUGO) ? ATTR_NONE : ATTR_RO)

struct fatx_boot_sector {
        __u32	magic;		/* "FATX" */
	__u32	volume_id;	/* Volume ID */
        __u32	cluster_size;	/* sectors/cluster */
	__u16	fats;		/* number of FATs */
	__u32	unknown;
};

struct fatx_dir_entry {
        __u8	name_length;	/* length of filename (bytes) */
	__u8	attr;		/* attribute bits */
        __s8	name[42];	/* filename */
	__u32	start;		/* first cluster */
	__u32	size;		/* file size (in bytes) */
	__u16	time,date;	/* time, date */
	__u16	ctime,cdate;	/* Creation time */
	__u16	atime,adate;	/* Last access time */
};

struct fatx_boot_fsinfo {
	__u32   signature1;	/* 0x41615252L */
	__u32   reserved1[120];	/* Nothing as far as I can tell */
	__u32   signature2;	/* 0x61417272L */
	__u32   free_clusters;	/* Free cluster count.  -1 if unknown */
	__u32   next_cluster;	/* Most recently allocated cluster.
				 * Unused under Linux. */
	__u32   reserved2[4];
};

struct fatx_cache {
	dev_t device; /* device number. 0 means unused. */
	int start_cluster; /* first cluster of the chain. */
	int file_cluster; /* cluster number in the file. */
	int disk_cluster; /* cluster number on disk. */
	struct fatx_cache *next; /* next cache entry */
};

#ifdef __KERNEL__

#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/nls.h>
#include <linux/fatx_fs_i.h>
#include <linux/fatx_fs_sb.h>
#include <asm/statfs.h>

static inline struct fatx_sb_info *FATX_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct fatx_inode_info *FATX_I(struct inode *inode)
{
	return container_of(inode, struct fatx_inode_info, vfs_inode);
}

/* fatx/cache.c */
extern int fatx_access(struct super_block *sb, int nr, int new_value);
extern unsigned long fatx_bmap(struct inode *inode,unsigned long sector);
extern void fatx_cache_init(void);
extern void fatx_cache_lookup(struct inode *inode, int cluster, int *f_clu,
			     int *d_clu);
extern void fatx_cache_add(struct inode *inode, int f_clu, int d_clu);
extern void fatx_cache_inval_inode(struct inode *inode);
extern void fatx_cache_inval_dev(dev_t device);
extern int fatx_get_cluster(struct inode *inode,int cluster);
extern int fatx_free(struct inode *inode, int skip);
extern void fatx_clusters_flush(struct super_block *sb);

/* fatx/dir.c */
extern struct file_operations fat_dir_operations;
extern int fatx_readdir(struct file *filp, void *dirent, filldir_t filldir);
extern int fatx_dir_empty(struct inode *dir);
extern int fatx_add_entries(struct inode *dir, int slots, struct buffer_head **bh,
			   struct fatx_dir_entry **de, loff_t *i_pos);
extern int fatx_new_dir(struct inode *dir, struct inode *parent);

/* fat/file.c */
extern struct file_operations fatx_file_operations;
extern struct inode_operations fatx_file_inode_operations;
extern ssize_t fatx_file_read(struct file *filp, char *buf, size_t count,
			     loff_t *ppos);
extern int fatx_get_block(struct inode *inode, sector_t iblock,
			 struct buffer_head *bh_result, int create);
extern ssize_t fatx_file_write(struct file *filp, const char __user *buf, size_t count,
			      loff_t *ppos);
extern void fatx_truncate(struct inode *inode);

/* fat/inode.c */
extern void fatx_hash_init(void);
extern void fatx_attach(struct inode *inode, loff_t i_pos);
extern void fatx_detach(struct inode *inode);
extern struct inode *fatx_iget(struct super_block *sb, loff_t i_pos);
extern struct inode *fatx_build_inode(
		struct super_block *sb,
		struct fatx_dir_entry *de, 
		loff_t i_pos, 
		int *res);
extern int fatx_fill_super(struct super_block *sb,void *data, int silent);

extern void fatx_delete_inode(struct inode *inode);
extern void fatx_clear_inode(struct inode *inode);
extern void fatx_put_super(struct super_block *sb);

typedef int (*fatx_boot_block_parse_func)(
		struct super_block *sb, 
		struct buffer_head *bh );
typedef void (*fatx_read_root_func)(struct inode *inode);

extern int fatx_statfs(struct super_block *sb, struct kstatfs *buf);
extern void fatx_write_inode(struct inode *inode, int wait);
extern int fatx_notify_change(struct dentry * dentry, struct iattr * attr);

extern struct address_space_operations fatx_aops;
extern spinlock_t fatx_inode_lock;


/* fatx/namei.c - these are for the xbox's FATX */
struct dentry *fatx_lookup(struct inode *dir,struct dentry *dentry, struct nameidata *nd);
extern int fatx_create(struct inode *dir, struct dentry *dentry, int mode, 
		struct nameidata *nd);
extern int fatx_rmdir(struct inode *dir, struct dentry *dentry);
extern int fatx_unlink(struct inode *dir, struct dentry *dentry);
extern int fatx_mkdir(struct inode *dir, struct dentry *dentry, int mode);
extern int fatx_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry);

/* fatx/fatxfs_syms.c */
extern unsigned int fatx_debug;

extern struct file_system_type fatx_fs_type;

extern int fatx_get_entry(struct inode *dir,loff_t *pos,struct buffer_head **bh,
		struct fatx_dir_entry **de,loff_t *i_pos );

extern int fatx_scan(struct inode *dir, const char *name, int name_length,
		struct buffer_head **res_bh,struct fatx_dir_entry **res_de,
		loff_t *i_pos);
		
/* miscelaneous support code for fatx fs */
extern int fatx_date_dos2unix( unsigned short, unsigned short );
extern void fatx_date_unix2dos(int unix_date, unsigned short *time, unsigned short *date);

static inline unsigned char fatx_tolower(struct nls_table *t, unsigned char c)
{
	unsigned char nc = t->charset2lower[c];

	return nc ? nc : c;
}

static inline unsigned char fatx_toupper(struct nls_table *t, unsigned char c)
{
	unsigned char nc = t->charset2upper[c];

	return nc ? nc : c;
}

static inline int fatx_strnicmp(struct nls_table *t, const unsigned char *s1,
		const unsigned char *s2, int len )
{
	while(len--) {
		if (fatx_tolower(t, *s1++) != fatx_tolower(t, *s2++))
			return 1;
	}
	return 0;
}

/* directory code for fatx fs */
extern int fatx_do_add_entry(struct inode *dir,	struct buffer_head **bh,
		struct fatx_dir_entry **de, loff_t *i_pos);
		
extern int fatx_dir_empty(struct inode *dir);
extern int fatx_subdirs(struct inode *dir);
		
extern struct file_operations fatx_dir_operations;
#endif /* __KERNEL__ */

extern int fatx_access(struct super_block *sb, int nr, int new_value);
extern void fatx_fs_panic(struct super_block *s,const char *msg);
extern struct buffer_head *fatx_extend_dir(struct inode *inode);
extern int fatx_add_cluster(struct inode *inode);
extern void lock_fatx(struct super_block *sb);
extern void unlock_fatx(struct super_block *sb);

#endif /* _LINUX_FATX_FS_H */
