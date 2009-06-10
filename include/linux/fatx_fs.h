#ifndef _LINUX_FATX_FS_H
#define _LINUX_FATX_FS_H

#include <asm/byteorder.h>
#define FATX_DPB	(FATX_DPS)	/* dir entries per block */
#define FATX_DPB_BITS	4		/* log2(FATX_DPB) */
#define FATX_DPS	(SECTOR_SIZE / sizeof(struct fatx_dir_entry))
#define FATX_DPS_BITS	4		/* log2(FATX_DPS) */

#define FATX_SUPER_MAGIC cpu_to_le32(0x58544146)

#define FATX32_MAX_NON_LFS     ((1UL<<32) - 1)
#define FATX16_MAX_NON_LFS     ((1UL<<30) - 1)

#define FATX_CLUSTER_SIZE            32
#define FATX_ROOT_INO	1	/* == MINIX_ROOT_INO */
#define FATX_DIR_BITS	6	/* log2(sizeof(struct fatx_dir_entry)) */

// /* attribute bits that are copied "as is" */
//  #define ATTR_UNUSED	(ATTR_ARCH | ATTR_SYS | ATTR_HIDDEN)
// /* bits that are used by the Windows 95/Windows NT extended FAT */
// #define ATTR_EXT	(ATTR_RO | ATTR_HIDDEN | ATTR_SYS)

#define FATX_IS_FREE(de) (((de)->name_length==DELETED_FLAG || (de)->name_length==0xFF))
#define FATX_END_OF_DIR(n) ((n)->name_length==0xFF)

/* directory limit */
#define FATX_MAX_DIR_ENTRIES	(256)
#define FATX_MAX_DIR_SIZE	(FATX_MAX_DIR_ENTRIES << FATX_DIR_BITS)

/* valid file mode bits */
#define FATX_VALID_MODE (S_IFREG | S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
/* Convert attribute bits and a mask to the UNIX mode. */
#define FATX_MKMODE(a, m) (m & (a & ATTR_RO ? S_IRUGO|S_IXUGO : S_IRWXUGO))

#define FATX_NAME	42	/* maximum name length */

/* bad cluster mark */
//#define BAD_FATX16	0xFFF7
//#define BAD_FATX32	0xFFFFFFF7

/* standard EOF */
//#define EOF_FATX16	0xFFFF
//#define EOF_FATX32	0xFFFFFFFF

#define FATX_ENT_FREE	(0)
#define FATX_ENT_BAD	(BAD_FAT32)
#define FATX_ENT_EOF	(EOF_FAT32)

/*
 * ioctl commands
 */
#define FATX_IOCTL_GET_ATTRIBUTES	_IOR('r', 0x10, __u32)
#define FATX_IOCTL_SET_ATTRIBUTES	_IOW('r', 0x11, __u32)

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

//struct fatx_slot_info {
//	loff_t i_pos;		/* on-disk position of directory entry */
//	loff_t slot_off;	/* offset for slot or de start */
//	struct fatx_dir_entry *de;
//	struct buffer_head *bh;
//};

extern int fatx_date_dos2unix(unsigned short time, unsigned short date);
extern void fatx_date_unix2dos(int unix_date, __le16 *time, __le16 *date);

#endif
