#ifndef _FATX_FS_SB
#define _FATX_FS_SB

/*
 *  FATX file system in-core superblock data
 *
 *  Written 2003 by Edgar Hucek and Lehner Franz
 *
 */

struct fatx_mount_options {
	uid_t fs_uid;
	gid_t fs_gid;
	unsigned short fs_umask;
	unsigned short codepage;  /* Codepage for shortname conversions */
	unsigned short shortname; /* flags for shortname display/create rule */
	unsigned char name_check; /* r = relaxed, n = normal, s = strict */
	unsigned char conversion; /* b = binary, t = text, a = auto */
	unsigned quiet:1;         /* set = fake successful chmods and chowns */
};

#define FATX_CACHE_NR    8 /* number of FAT cache */

struct fatx_sb_info {
	unsigned short sec_per_clus; /* sectors/cluster */
	unsigned short cluster_bits; /* log2(cluster_size) */
	unsigned int cluster_size;   /* cluster size */
	unsigned char fats,fat_bits; /* number of FATs, FAT bits (12 or 16) */
	unsigned short fat_start;
	unsigned long fat_length;    /* FAT start & length (sec.) */
	unsigned long dir_start;
	unsigned short dir_entries;  /* root dir start & entries */
	unsigned long data_start;    /* first data sector */
	unsigned long clusters;      /* number of clusters */
	unsigned long root_cluster;  /* first cluster of the root directory */
	unsigned long fsinfo_sector; /* FAT32 fsinfo offset from start of disk */
	struct semaphore fatx_lock;
	int prev_free;               /* previously returned free cluster number */
	int free_clusters;           /* -1 if undefined */
	struct fatx_mount_options options;
	struct nls_table *nls_disk;  /* Codepage used on disk */
	struct nls_table *nls_io;    /* Charset used for input and display */
	void *dir_ops;		     /* Opaque; default directory operations */
	void *private_data;
	int dir_per_block;	     /* dir entries per block */
	int dir_per_block_bits;	     /* log2(dir_per_block) */

	spinlock_t cache_lock;
};

#endif
