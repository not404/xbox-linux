/*
 *  fs/partitions/xbox.c
 *
 *  //TODO: find and insert GPL notice
 *
 *  June 2002 by SpeedBump:	initial implementation
 */

#include <linux/config.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/blkdev.h>

#ifdef CONFIG_BLK_DEV_IDE
#include <linux/ide.h>	/* IDE xlate */
#endif /* CONFIG_BLK_DEV_IDE */

#include <asm/system.h>

#include "check.h"
#include "xbox.h"

#define DEBUG

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

static int
xbox_sig_string_match(	struct block_device *bdev, 
			unsigned long at_sector,
			char *expect )
{
	Sector sect;
	int retv;
	char *data;

	data = read_dev_sector(bdev, at_sector, &sect);
	
	if (!data) return 0;

	if (*(u32*)expect == *(u32*)data) retv = 1; else retv = 0;
	
	put_dev_sector(sect);
	
	return retv;
}

static inline int
xbox_drive_detect(struct block_device *bdev)
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
	if (	(xbox_sig_string_match(bdev,XBOX_SECTOR_MAGIC ,"BRFR")) &&
		(xbox_sig_string_match(bdev,XBOX_SECTOR_SYSTEM,"FATX")) &&
		(xbox_sig_string_match(bdev,XBOX_SECTOR_STORE ,"FATX"))) {
		return 1; //success
	}
	
	return -1; //failed
}

int xbox_partition(struct parsed_partitions *state, struct block_device *bdev)
{
	ide_drive_t *drive = bdev->bd_disk->private_data;
	u64 last_sector = drive->capacity64 - 1;
	int retv;
	int start,length,num;
	
	retv = xbox_drive_detect(bdev);
	if (retv > 0) {
		/* trying to find the first free partition */

		state->next = 50;
		printk("\n");
		start = XBOX_SECTOR_STORE; 
		length = XBOX_SECTORS_STORE;
		put_partition(state,state->next++,start ,length);

		start = XBOX_SECTOR_SYSTEM; 
		length = XBOX_SECTORS_SYSTEM;
		put_partition(state,state->next++,start ,length);
		
		start = XBOX_SECTOR_CACHE1; 
		length = XBOX_SECTORS_CACHE1;
		put_partition(state,state->next++,start ,length);
		
		start = XBOX_SECTOR_CACHE2; 
		length = XBOX_SECTORS_CACHE2;
		put_partition(state,state->next++,start ,length);
		
		start = XBOX_SECTOR_CACHE3; 
		length = XBOX_SECTORS_CACHE3;
		put_partition(state,state->next++,start ,length);
		
		//there are some questions about the usage of any "extra" sectors...
		//I'll map everything after the end to an aditional sector.
		if (XBOX_SECTOR_EXTEND < last_sector) {
			if (xbox_sig_string_match(bdev,XBOX_SECTOR_EXTEND ,"FATX")) {
				start = XBOX_SECTOR_EXTEND; 
				length = last_sector - XBOX_SECTOR_EXTEND; 
				num = state->next - 1;
				put_partition(state,state->next++,start ,length);
			}
		}
		printk("\n");
		return 1;
	}
	
	return 0;
}
