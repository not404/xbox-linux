/*
 * fs/partitions/xbox.c
 * Xbox disk partition support.
 *
 * Copyright (C) 2002  John Scott Tillman <speedbump@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/errno.h>
#include <linux/genhd.h>
#include <linux/kernel.h>

#include "check.h"
#include "xbox.h"


/*
 * The native Xbox kernel makes use of an implicit partitioning
 * scheme. Partition locations and sizes on-disk are hard-coded.
 */
#define XBOX_CONFIG_START	0x00000000L
#define XBOX_CACHE1_START	0x00000400L
#define XBOX_CACHE2_START	0x00177400L
#define XBOX_CACHE3_START	0x002EE400L
#define XBOX_SYSTEM_START	0x00465400L
#define XBOX_DATA_START		0x0055F400L
#define XBOX_EXTEND_START	0x00EE8AB0L

#define XBOX_CONFIG_SIZE	(XBOX_CACHE1_START - XBOX_CONFIG_START)
#define XBOX_CACHE1_SIZE	(XBOX_CACHE2_START - XBOX_CACHE1_START)
#define XBOX_CACHE2_SIZE	(XBOX_CACHE3_START - XBOX_CACHE2_START)
#define XBOX_CACHE3_SIZE	(XBOX_SYSTEM_START - XBOX_CACHE3_START)
#define XBOX_SYSTEM_SIZE	(XBOX_DATA_START - XBOX_SYSTEM_START)
#define XBOX_DATA_SIZE		(XBOX_EXTEND_START - XBOX_DATA_START)

#define XBOX_MAGIC_SECT		3L


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
	return (xbox_check_magic(bdev, XBOX_MAGIC_SECT, "BRFR") &&
		xbox_check_magic(bdev, XBOX_SYSTEM_START, "FATX") &&
		xbox_check_magic(bdev, XBOX_DATA_START, "FATX")) ?
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

	put_partition(state, slot++, XBOX_DATA_START, XBOX_DATA_SIZE);
	put_partition(state, slot++, XBOX_SYSTEM_START, XBOX_SYSTEM_SIZE);
	put_partition(state, slot++, XBOX_CACHE1_START, XBOX_CACHE1_SIZE);
	put_partition(state, slot++, XBOX_CACHE2_START, XBOX_CACHE2_SIZE);
	put_partition(state, slot++, XBOX_CACHE3_START, XBOX_CACHE3_SIZE);

	/*
	 * Xbox HDDs come in two sizes - 8GB and 10GB. The native Xbox kernel
	 * will only acknowledge the first 8GB, regardless of actual disk
	 * size. For disks larger than 8GB, anything above that limit is made
	 * available as a seperate partition.
	 */
	last = bdev->bd_disk->capacity;
	if (last == XBOX_EXTEND_START)
		goto out;

	printk(" <");
	size = last - XBOX_EXTEND_START;
	put_partition(state, slot++, XBOX_EXTEND_START, size);
	printk(" >");

out:
	printk("\n");
	return 1;
}
