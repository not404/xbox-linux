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
#define CF_LE_W(v)	le16_to_cpu(v)
#define CF_LE_L(v)	le32_to_cpu(v)
#define CT_LE_W(v)	cpu_to_le16(v)
#define CT_LE_L(v)	cpu_to_le32(v)


#define FATX_SUPER_MAGIC cpu_to_le32(0x58544146)

#define FATX32_MAX_NON_LFS     ((1UL<<32) - 1)
#define FATX16_MAX_NON_LFS     ((1UL<<30) - 1)

#define CLUSTER_SIZE            32
#define FATX_ROOT_INO	1	/* == MINIX_ROOT_INO */
#define FATX_DIR_BITS	6	/* log2(sizeof(struct fatx_dir_entry)) */

/* directory limit */
//#define FAT_MAX_DIR_ENTRIES	(65536)
//#define FAT_MAX_DIR_SIZE	(FAT_MAX_DIR_ENTRIES << FATX_DIR_BITS)

#define ATTR_NONE	0	/* no attribute bits */
#define ATTR_RO		1	/* read-only */
#define ATTR_HIDDEN	2	/* hidden */
#define ATTR_SYS	4	/* system */
#define ATTR_DIR	16	/* directory */
#define ATTR_ARCH	32	/* archived */

/* attribute bits that are copied "as is" */
#define ATTR_UNUSED	(ATTR_ARCH | ATTR_SYS | ATTR_HIDDEN)
/* bits that are used by the Windows 95/Windows NT extended FAT */
#define ATTR_EXT	(ATTR_RO | ATTR_HIDDEN | ATTR_SYS)

#define CASE_LOWER_BASE	8	/* base is lower case */
#define CASE_LOWER_EXT	16	/* extension is lower case */

#define DELETED_FLAG	0xe5	/* marks file as deleted when in name[0] */
#define FATX_IS_FREE(de) (((de)->name_length==DELETED_FLAG || (de)->name_length==0xFF))
#define FATX_END_OF_DIR(n) ((n)->name_length==0xFF)
#define IS_FREE(de) ((de)->name_length==DELETED_FLAG)
//#define IS_FREE(n)	(!*(n) || *(n) == DELETED_FLAG)

/* directory limit */
#define FATX_MAX_DIR_ENTRIES	(256)
#define FATX_MAX_DIR_SIZE	(FATX_MAX_DIR_ENTRIES << FATX_DIR_BITS)

/* valid file mode bits */
#define FATX_VALID_MODE (S_IFREG | S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
/* Convert attribute bits and a mask to the UNIX mode. */
#define FATX_MKMODE(a, m) (m & (a & ATTR_RO ? S_IRUGO|S_IXUGO : S_IRWXUGO))

#define FATX_NAME	42	/* maximum name length */

/* start of data cluster's entry (number of reserved clusters) */
#define FAT_START_ENT	2

/* bad cluster mark */
#define BAD_FAT16	0xFFF7
#define BAD_FAT32	0xFFFFFFF7

/* standard EOF */
#define EOF_FAT16	0xFFFF
#define EOF_FAT32	0xFFFFFFFF

#define FAT_ENT_FREE	(0)
#define FAT_ENT_BAD	(BAD_FAT32)
#define FAT_ENT_EOF	(EOF_FAT32)

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

struct fatx_slot_info {
	loff_t i_pos;		/* on-disk position of directory entry */
	loff_t slot_off;	/* offset for slot or de start */
	struct fatx_dir_entry *de;
	struct buffer_head *bh;
};

#ifdef __KERNEL__

#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/nls.h>
#include <linux/fs.h>

struct fatx_mount_options {
	uid_t fs_uid;
	gid_t fs_gid;
	unsigned short fs_fmask;
	unsigned short fs_dmask;
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
	unsigned char fatxs,fatx_bits; /* number of FATs, FAT bits (16 or 32) */
	unsigned short fatx_start;
	unsigned long fatx_length;    /* FAT start & length (sec.) */
	unsigned long dir_start;
	unsigned short dir_entries;  /* root dir start & entries */
	unsigned long data_start;    /* first data sector */
	unsigned long max_cluster;   /* maximum cluster number */
	unsigned long root_cluster;  /* first cluster of the root directory */
	unsigned long fsinfo_sector; /* sector number of FAT32 fsinfo */
	struct semaphore fatx_lock;
	unsigned int prev_free;      /* previously allocated cluster number */
	unsigned int free_clusters;  /* -1 if undefined */
	struct fatx_mount_options options;
	struct nls_table *nls_disk;  /* Codepage used on disk */
	struct nls_table *nls_io;    /* Charset used for input and display */
	void *dir_ops;		     /* Opaque; default directory operations */
	int dir_per_block;	     /* dir entries per block */
	int dir_per_block_bits;	     /* log2(dir_per_block) */

	int fatxent_shift;
	struct fatxent_operations *fatxent_ops;

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
	int i_ctime_ms;         /* unused change time in milliseconds */
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

/* Return the FAT attribute byte for this inode */
static inline u8 fatx_attr(struct inode *inode)
{
	return ((inode->i_mode & S_IWUGO) ? ATTR_NONE : ATTR_RO) |
		(S_ISDIR(inode->i_mode) ? ATTR_DIR : ATTR_NONE) |
		FATX_I(inode)->i_attrs;
}

static inline sector_t fatx_clus_to_blknr(struct fatx_sb_info *sbi, __s64 clus)
{
	return ((sector_t)clus - FAT_START_ENT) * sbi->sec_per_clus
		+ sbi->data_start;
}

/* fatx/cache.c */
extern void fatx_cache_inval_inode(struct inode *inode);
extern __s64 fatx_get_cluster(struct inode *inode, __s64 cluster,
			   int *fclus, int *dclus);
extern int fatx_bmap(struct inode *inode, sector_t sector, sector_t *phys);

/* fatx/dir.c */
extern struct file_operations fatx_dir_operations;
extern int fatx_search_long(struct inode *inode, const unsigned char *name,
			   int name_len, struct fatx_slot_info *sinfo);
extern int fatx_dir_empty(struct inode *dir);
extern int fatx_subdirs(struct inode *dir);
extern int fatx_scan(struct inode *dir, const unsigned char *name,
		    struct fatx_slot_info *sinfo);
extern int fatx_get_dotdot_entry(struct inode *dir, struct buffer_head **bh,
				struct fatx_dir_entry **de, loff_t *i_pos);
extern int fatx_alloc_new_dir(struct inode *dir, struct timespec *ts);
extern int fatx_add_entries(struct inode *dir, void *slots, struct fatx_slot_info *sinfo);
extern int fatx_remove_entries(struct inode *dir, struct fatx_slot_info *sinfo);

/* fatx/fatxent.c */
struct fatx_entry {
	int entry;
	union {
		__le16 *ent16_p;
		__u32 *ent32_p;
	} u;
	int nr_bhs;
	struct buffer_head *bhs[2];
};

static inline void fatxent_init(struct fatx_entry *fatxent)
{
	fatxent->nr_bhs = 0;
	fatxent->entry = 0;
	fatxent->u.ent32_p = NULL;
	fatxent->bhs[0] = fatxent->bhs[1] = NULL;
}

static inline void fatxent_set_entry(struct fatx_entry *fatxent, int entry)
{
	fatxent->entry = entry;
	fatxent->u.ent32_p = NULL;
}

static inline void fatxent_brelse(struct fatx_entry *fatxent)
{
	int i;
	fatxent->u.ent32_p = NULL;
	for (i = 0; i < fatxent->nr_bhs; i++)
		brelse(fatxent->bhs[i]);
	fatxent->nr_bhs = 0;
	fatxent->bhs[0] = fatxent->bhs[1] = NULL;
}

extern void fatx_ent_access_init(struct super_block *sb);
extern __s64 fatx_ent_read(struct inode *inode, struct fatx_entry *fatxent,
			__s64 entry);
extern int fatx_ent_write(struct inode *inode, struct fatx_entry *fatxent,
			 __s64 new, int wait);
extern __s64 fatx_alloc_clusters(struct inode *inode, __s64 *cluster,
			      int nr_cluster);
extern __s64 fatx_free_clusters(struct inode *inode, __s64 cluster);
extern __s64 fatx_count_free_clusters(struct super_block *sb);

/* fatx/file.c */
extern int fatx_generic_ioctl(struct inode *inode, struct file *filp,
			     unsigned int cmd, unsigned long arg);
extern struct file_operations fatx_file_operations;
extern struct inode_operations fatx_file_inode_operations;
extern int fatx_notify_change(struct dentry * dentry, struct iattr * attr);
extern void fatx_truncate(struct inode *inode);

/* fatx/inode.c */
extern void fatx_attach(struct inode *inode, loff_t i_pos);
extern void fatx_detach(struct inode *inode);
extern struct inode *fatx_iget(struct super_block *sb, loff_t i_pos);
extern struct inode *fatx_build_inode(struct super_block *sb,
			struct fatx_dir_entry *de, loff_t i_pos);
extern int fatx_sync_inode(struct inode *inode);
extern int fatx_fill_super_inode(struct super_block *sb, void *data, int silent,
			struct inode_operations *fs_dir_inode_ops);
extern unsigned int fatx_debug;

/* fatx/misc.c */
extern void fatx_fs_panic(struct super_block *s, const char *fmt, ...);
extern __s64 fatx_chain_add(struct inode *inode, int new_dclus, int nr_cluster);
extern int fatx_date_dos2unix(unsigned short time, unsigned short date);
extern void fatx_date_unix2dos(int unix_date, __le16 *time, __le16 *date);
extern int fatx_sync_bhs(struct buffer_head **bhs, int nr_bhs);

/* fatx/namei.c */
extern void fatx_printname(const char *name, int length);
extern struct super_block *fatx_get_sb(struct file_system_type *fs_type,
					int flags, const char *dev_name,
					void *data);

#endif /* __KERNEL__ */

#endif
