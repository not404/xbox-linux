/*
 *  fs/partitions/xbox.c
 *
 *  //TODO: find and insert GPL notice
 *
 *  June 2002 by SpeedBump:	initial implementation
 */

#include <linux/errno.h>
#include <linux/genhd.h>
#include <linux/kernel.h>

#include "check.h"
#include "xbox.h"

#define XBOX_SECTOR_STORE	(0x0055F400L)
#define XBOX_SECTOR_SYSTEM	(0x00465400L)
#define XBOX_SECTOR_CONFIG	(0x00000000L)
#define XBOX_SECTOR_CACHE1	(0x00000400L)
#define XBOX_SECTOR_CACHE2	(0x00177400L)
#define XBOX_SECTOR_CACHE3	(0x002EE400L)
#define XBOX_SECTOR_EXTEND	(0x00EE8AB0L)
#define XBOX_SECTORS_CONFIG	(XBOX_SECTOR_CACHE1 - XBOX_SECTOR_CONFIG)

#define XBOX_SECTORS_STORE	(XBOX_SECTOR_EXTEND - XBOX_SECTOR_STORE)
#define XBOX_SECTORS_SYSTEM	(XBOX_SECTOR_STORE  - XBOX_SECTOR_SYSTEM)
#define XBOX_SECTORS_CACHE1	(XBOX_SECTOR_CACHE2 - XBOX_SECTOR_CACHE1)
#define XBOX_SECTORS_CACHE2	(XBOX_SECTOR_CACHE3 - XBOX_SECTOR_CACHE2)
#define XBOX_SECTORS_CACHE3	(XBOX_SECTOR_SYSTEM - XBOX_SECTOR_CACHE3)
#define XBOX_SECTORS_CONFIG	(XBOX_SECTOR_CACHE1 - XBOX_SECTOR_CONFIG)

#define XBOX_SECTOR_MAGIC	(3L)

static int xbox_check_magic(struct block_device *bdev, sector_t at_sect,
		char *magic)
{
	Sector sect;
	char *data;
	int ret;

	data = read_dev_sector(bdev, at_sect, &sect);
	if (!data)
		return -1;

	/* Ick! */
	ret = (*(u32*)data == *(u32*)magic) ? 1 : 0;
	put_dev_sector(sect);

	return ret;
}

static inline int xbox_drive_detect(struct block_device *bdev)
{
	/** 
	* "BRFR" is apparently the magic number in the config area
	* the others are just paranoid checks to assure the expected
	* "FATX" tags for the other xbox partitions
	*
	* the odds against a non-xbox drive having random data to match is
	* astronomical...but it's possible I guess...you should only include
	* this check if you actually *have* an xbox drive...since it has to
	* be detected first
	*
	* @see check.c
	*/
	return (xbox_check_magic(bdev, XBOX_SECTOR_MAGIC, "BRFR") &&
		xbox_check_magic(bdev, XBOX_SECTOR_SYSTEM, "FATX") &&
		xbox_check_magic(bdev, XBOX_SECTOR_STORE, "FATX")) ?
		0 : -ENODEV;
}

int xbox_partition(struct parsed_partitions *state, struct block_device *bdev)
{
	int slot, err;
	sector_t last, size;

	err = xbox_drive_detect(bdev);
	if (err)
		return err;

	slot = 50;
	printk(" [xbox]");

	put_partition(state, slot++, XBOX_SECTOR_CACHE1, XBOX_SECTORS_CACHE1);
	put_partition(state, slot++, XBOX_SECTOR_CACHE2, XBOX_SECTORS_CACHE2);
	put_partition(state, slot++, XBOX_SECTOR_CACHE3, XBOX_SECTORS_CACHE3);
	put_partition(state, slot++, XBOX_SECTOR_SYSTEM, XBOX_SECTORS_SYSTEM);
	put_partition(state, slot++, XBOX_SECTOR_STORE, XBOX_SECTORS_STORE);

	/*
	 * Xbox HDDs come in two sizes - 8GB and 10GB. The native Xbox kernel
	 * will only acknowledge the first 8GB, regardless of actual disk
	 * size. For disks larger than 8GB, anything above that limit is made
	 * available as a seperate partition.
	 */
	last = bdev->bd_disk->capacity;
	if (last == XBOX_SECTOR_EXTEND)
		goto out;

	printk(" <");
	size = last - XBOX_SECTOR_EXTEND;
	put_partition(state, slot++, XBOX_SECTOR_EXTEND, size);
	printk(" >");

out:
	printk("\n");
	return 1;
}
