#ifndef _LINUX_FATX_FS_H
#define _LINUX_FATX_FS_H

/*
 * The MS-DOS filesystem constants/structures
 */
#include <asm/byteorder.h>

#define SECTOR_SIZE	512		/* sector size (bytes) */
#define SECTOR_BITS	9		/* log2(SECTOR_SIZE) */
#define FATX_DPB	(FATX_DPS)	/* dir entries per block */
#define FATX_DPB_BITS	4		/* log2(FATX_DPB) */
#define FATX_DPS	(SECTOR_SIZE / sizeof(struct fatx_dir_entry))
#define FATX_DPS_BITS	4		/* log2(FATX_DPS) */

#define FATX_SUPER_MAGIC cpu_to_le32(0x58544146)

#define FATX32_MAX_NON_LFS     ((1UL<<32) - 1)
#define FATX16_MAX_NON_LFS     ((1UL<<30) - 1)

#define CLUSTER_SIZE		32
#define FATX_DIR_BITS		6	/* log2(sizeof(struct fatx_dir_entry)) */

/* directory limit */
//#define FAT_MAX_DIR_ENTRIES	(65536)
//#define FAT_MAX_DIR_SIZE	(FAT_MAX_DIR_ENTRIES << FATX_DIR_BITS)

#define ATTR_NONE	0	/* no attribute bits */
#define ATTR_RO		1	/* read-only */
#define ATTR_HIDDEN	2	/* hidden */
#define ATTR_SYS	4	/* system */
#define ATTR_DIR	16	/* directory */
#define ATTR_ARCH	0	/* archived */

/* attribute bits that are copied "as is" */
#define ATTR_UNUSED	(ATTR_ARCH | ATTR_SYS | ATTR_HIDDEN)

#define DELETED_FLAG	0xe5	/* marks file as deleted when in name[0] */
//#define IS_FREE(n)      (!*(n) || *(const unsigned char *)(n) == DELETED_FLAG)
#define FATX_IS_FREE(de) ((de)->name_length==DELETED_FLAG)
#define FATX_END_OF_DIR(n) ((n)->name_length==0xFF)

/* valid file mode bits */
#define FATX_VALID_MODE (S_IFREG | S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
/* Convert attribute bits and a mask to the UNIX mode. */
#define FATX_MKMODE(a, m) (m & (a & ATTR_RO ? S_IRUGO|S_IXUGO : S_IRWXUGO))
/* Convert the UNIX mode to MS-DOS attribute bits. */
#define FATX_MKATTR(m)	((m & S_IWUGO) ? ATTR_NONE : ATTR_RO)

#define FATX_NAME	42	/* maximum name length */

/* bad cluster mark */
#define BAD_FAT16	0xFFF7
#define BAD_FAT32	0xFFFFFFF7
#define BAD_FAT(s)	(FATX_SB(s)->fatx_bits == 32 ? BAD_FAT32 : BAD_FAT16)

/* standard EOF */
#define EOF_FAT16	0xFFF8
#define EOF_FAT32	0xFFFFFFF8
#define EOF_FAT(s)	(FATX_SB(s)->fatx_bits == 32 ? EOF_FAT32 : EOF_FAT16)

#define FAT_ENT_FREE	(0)
#define FAT_ENT_BAD	(BAD_FAT32)
#define FAT_ENT_EOF	(EOF_FAT32)
//#define FAT_ENT_EOF	(0x0FFFFFF8)

#define FATX_ROOT_INO		1
#define FAT_START_ENT		2

struct fatx_boot_sector {
        __u32	magic;		/* "FATX" */
	__u32	volume_id;	/* Volume ID */
        __u32	cluster_size;	/* sectors/cluster */
	__u16	fats;		/* number of FATs */
	__u32	unknown;
};

struct fatx_dir_entry {
	__u8    name_length;    /* length of filename (bytes) */
	__u8    attr;           /* attribute bits */
	__u8    name[FATX_NAME];       /* filename */
	__u32   start;          /* first cluster */
	__u32   size;           /* file size (in bytes) */
	__u16   time,date;      /* time, date */
	__u16   ctime,cdate;    /* Creation time */
	__u16   atime,adate;    /* Last access time */
};

#ifdef __KERNEL__

#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/nls.h>
#include <linux/fs.h>

struct fatx_mount_options {
	uid_t fs_uid;
	gid_t fs_gid;
	unsigned short fs_umask;
	unsigned short codepage;  /* Codepage for shortname conversions */
	char *iocharset;          /* Charset used for filename input/display */
	unsigned quiet:1;         /* set = fake successful chmods and chowns */
};

#define FAT_HASH_BITS	8
#define FAT_HASH_SIZE	(1UL << FAT_HASH_BITS)
#define FAT_HASH_MASK	(FAT_HASH_SIZE-1)

/*
 * MS-DOS file system in-core superblock data
 */
struct fatx_sb_info {
	unsigned short sec_per_clus; /* sectors/cluster */
	unsigned short cluster_bits; /* log2(cluster_size) */
	unsigned int cluster_size;   /* cluster size */
	unsigned char fatxs,fatx_bits; /* number of FATs, FAT bits (12 or 16) */
	unsigned short fatx_start;
	unsigned long fatx_length;    /* FAT start & length (sec.) */
	unsigned long dir_start;
	unsigned short dir_entries;  /* root dir start & entries */
	unsigned long data_start;    /* first data sector */
	unsigned long max_cluster;   /* maximum cluster number */
	unsigned long root_cluster;  /* first cluster of the root directory */
	unsigned long fsinfo_sector; /* sector number of FAT32 fsinfo */
	struct semaphore fatx_lock;
	int prev_free;               /* previously allocated cluster number */
	int free_clusters;           /* -1 if undefined */
	struct fatx_mount_options options;
	struct nls_table *nls_disk;  /* Codepage used on disk */
	struct nls_table *nls_io;    /* Charset used for input and display */
	void *dir_ops;		     /* Opaque; default directory operations */
	int dir_per_block;	     /* dir entries per block */
	int dir_per_block_bits;	     /* log2(dir_per_block) */

	spinlock_t inode_hash_lock;
	struct hlist_head inode_hashtable[FAT_HASH_SIZE];
};

#define FAT_CACHE_VALID	0	/* special case for valid cache */

/*
 * MS-DOS file system inode data in memory
 */
struct fatx_inode_info {
	spinlock_t cache_lru_lock;
	struct list_head cache_lru;
	int nr_caches;
	/* for avoiding the race between fatx_free() and fatx_get_cluster() */
	unsigned int cache_valid_id;

	loff_t mmu_private;
	int i_start;		/* first cluster or 0 */
	int i_logstart;		/* logical first cluster */
	int i_attrs;		/* unused attribute bits */
	int i_ctime_ms;		/* unused change time in milliseconds */
	loff_t i_pos;		/* on-disk position of directory entry or 0 */
	struct hlist_node i_fatx_hash;	/* hash by i_location */
	struct inode vfs_inode;
};

static inline struct fatx_sb_info *FATX_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct fatx_inode_info *FATX_I(struct inode *inode)
{
	return container_of(inode, struct fatx_inode_info, vfs_inode);
}

static inline sector_t fatx_clus_to_blknr(struct fatx_sb_info *sbi, int clus)
{
	return ((sector_t)clus - FAT_START_ENT) * sbi->sec_per_clus
		+ sbi->data_start;
}

static inline void fatx16_towchar(wchar_t *dst, const __u8 *src, size_t len)
{
#ifdef __BIG_ENDIAN
	while (len--) {
		*dst++ = src[0] | (src[1] << 8);
		src += 2;
	}
#else
	memcpy(dst, src, len * 2);
#endif
}

static inline void fatxwchar_to16(__u8 *dst, const wchar_t *src, size_t len)
{
#ifdef __BIG_ENDIAN
	while (len--) {
		dst[0] = *src & 0x00FF;
		dst[1] = (*src & 0xFF00) >> 8;
		dst += 2;
		src++;
	}
#else
	memcpy(dst, src, len * 2);
#endif
}

/* fatx/cache.c */
extern void fatx_cache_inval_inode(struct inode *inode);
extern __s64 fatx_access(struct super_block *sb, int nr, __s64 new_value);
extern __s64 fatx_get_cluster(struct inode *inode, __s64 cluster,
			   int *fclus, int *dclus);
extern unsigned long fatx_bmap(struct inode *inode, sector_t sector, sector_t *phys);

/* fatx/dir.c */
extern struct file_operations fatx_dir_operations;
extern int fatx_search_long(struct inode *inode, const unsigned char *name,
			   int name_len, int anycase,
			   loff_t *spos, loff_t *lpos);
extern int fatx_add_entries(struct inode *dir, 
			   struct buffer_head **bh,
			   struct fatx_dir_entry **de, loff_t *i_pos);
extern int fatx_new_dir(struct inode *dir, struct inode *parent);
extern int fatx_dir_empty(struct inode *dir);
extern int fatx_subdirs(struct inode *dir);
extern int fatx_scan(struct inode *dir, const unsigned char *name,
		    struct buffer_head **res_bh,
		    struct fatx_dir_entry **res_de, loff_t *i_pos);

/* fatx/file.c */
extern struct file_operations fatx_file_operations;
extern struct inode_operations fatx_file_inode_operations;
extern int fatx_notify_change(struct dentry * dentry, struct iattr * attr);
extern void fatx_truncate(struct inode *inode);

/* fatx/inode.c */
extern void fatx_attach(struct inode *inode, loff_t i_pos);
extern void fatx_detach(struct inode *inode);
extern struct inode *fatx_iget(struct super_block *sb, loff_t i_pos);
extern struct inode *fatx_build_inode(struct super_block *sb,
			struct fatx_dir_entry *de, loff_t i_pos, int *res);
int fatx_fill_super_inode(struct super_block *sb, void *data, int silent,
		   struct inode_operations *fs_dir_inode_ops);
extern unsigned int debug;

/* fatx/misc.c */
extern int fatx_date_dos2unix(unsigned short time,unsigned short date);
extern void fatx_fs_panic(struct super_block *s, const char *fmt, ...);
extern void lock_fatx(struct super_block *sb);
extern void unlock_fatx(struct super_block *sb);
extern void fatx_clusters_flush(struct super_block *sb);
extern __s64 fatx_add_cluster(struct inode *inode);
extern void fatx_date_unix2dos(int unix_date, __le16 *time, __le16 *date);
extern int fatx__get_entry(struct inode *dir, loff_t *pos,
			  struct buffer_head **bh,
			  struct fatx_dir_entry **de, loff_t *i_pos);
static __inline__ int fatx_get_entry(struct inode *dir, loff_t *pos,
				    struct buffer_head **bh,
				    struct fatx_dir_entry **de, loff_t *i_pos) 
{
	/* Fast stuff first */
	if (*bh && *de &&
		(*de - (struct fatx_dir_entry *)(*bh)->b_data) < FATX_SB(dir->i_sb)->dir_per_block - 1) {
			*pos += sizeof(struct fatx_dir_entry);
			(*de)++;
			(*i_pos)++;
			return 0;
	}
	return fatx__get_entry(dir, pos, bh, de, i_pos);
}
/* fatx/namei.c */
extern struct super_block *fatx_get_sb(struct file_system_type *fs_type,
					int flags, const char *dev_name,
					void *data);
#endif /* __KERNEL__ */

#endif
