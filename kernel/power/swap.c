/*
 * linux/kernel/power/swap.c
 *
 * This file provides functions for reading the suspend image from
 * and writing it to a swap partition.
 *
 * Copyright (C) 1998,2001-2005 Pavel Machek <pavel@suse.cz>
 * Copyright (C) 2006 Rafael J. Wysocki <rjw@sisk.pl>
 *
 * This file is released under the GPLv2.
 *
 */

#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/file.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/genhd.h>
#include <linux/device.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pm.h>

#include "power.h"

extern char resume_file[];

#define SWSUSP_SIG	"S1SUSPEND"

static struct swsusp_header {
	char reserved[PAGE_SIZE - 20 - sizeof(swp_entry_t)];
	swp_entry_t image;
	char	orig_sig[10];
	char	sig[10];
} __attribute__((packed, aligned(PAGE_SIZE))) swsusp_header;

/*
 * Saving part...
 */

static unsigned short root_swap = 0xffff;

static int mark_swapfiles(swp_entry_t start)
{
	int error;

	rw_swap_page_sync(READ, swp_entry(root_swap, 0),
			  virt_to_page((unsigned long)&swsusp_header), NULL);
	if (!memcmp("SWAP-SPACE",swsusp_header.sig, 10) ||
	    !memcmp("SWAPSPACE2",swsusp_header.sig, 10)) {
		memcpy(swsusp_header.orig_sig,swsusp_header.sig, 10);
		memcpy(swsusp_header.sig,SWSUSP_SIG, 10);
		swsusp_header.image = start;
		error = rw_swap_page_sync(WRITE, swp_entry(root_swap, 0),
				virt_to_page((unsigned long)&swsusp_header),
				NULL);
	} else {
		pr_debug("swsusp: Partition is not swap space.\n");
		error = -ENODEV;
	}
	return error;
}

/**
 *	swsusp_swap_check - check if the resume device is a swap device
 *	and get its index (if so)
 */

static int swsusp_swap_check(void) /* This is called before saving image */
{
	int res = swap_type_of(swsusp_resume_device);

	if (res >= 0) {
		root_swap = res;
		return 0;
	}
	return res;
}

/**
 *	write_page - Write one page to given swap location.
 *	@buf:		Address we're writing.
 *	@offset:	Offset of the swap page we're writing to.
 *	@bio_chain:	Link the next write BIO here
 */

static int write_page(void *buf, unsigned long offset, struct bio **bio_chain)
{
	swp_entry_t entry;
	int error = -ENOSPC;

	if (offset) {
		struct page *page = virt_to_page(buf);

		if (bio_chain) {
			/*
			 * Whether or not we successfully allocated a copy page,
			 * we take a ref on the page here.  It gets undone in
			 * wait_on_bio_chain().
			 */
			struct page *page_copy;
			page_copy = alloc_page(GFP_ATOMIC);
			if (page_copy == NULL) {
				WARN_ON_ONCE(1);
				bio_chain = NULL;	/* Go synchronous */
				get_page(page);
			} else {
				memcpy(page_address(page_copy),
					page_address(page), PAGE_SIZE);
				page = page_copy;
			}
		}
		entry = swp_entry(root_swap, offset);
		error = rw_swap_page_sync(WRITE, entry, page, bio_chain);
	}
	return error;
}

/*
 *	The swap map is a data structure used for keeping track of each page
 *	written to a swap partition.  It consists of many swap_map_page
 *	structures that contain each an array of MAP_PAGE_SIZE swap entries.
 *	These structures are stored on the swap and linked together with the
 *	help of the .next_swap member.
 *
 *	The swap map is created during suspend.  The swap map pages are
 *	allocated and populated one at a time, so we only need one memory
 *	page to set up the entire structure.
 *
 *	During resume we also only need to use one swap_map_page structure
 *	at a time.
 */

#define MAP_PAGE_ENTRIES	(PAGE_SIZE / sizeof(long) - 1)

struct swap_map_page {
	unsigned long		entries[MAP_PAGE_ENTRIES];
	unsigned long		next_swap;
};

/**
 *	The swap_map_handle structure is used for handling swap in
 *	a file-alike way
 */

struct swap_map_handle {
	struct swap_map_page *cur;
	unsigned long cur_swap;
	struct bitmap_page *bitmap;
	unsigned int k;
};

static void release_swap_writer(struct swap_map_handle *handle)
{
	if (handle->cur)
		free_page((unsigned long)handle->cur);
	handle->cur = NULL;
	if (handle->bitmap)
		free_bitmap(handle->bitmap);
	handle->bitmap = NULL;
}

static void show_speed(struct timeval *start, struct timeval *stop,
			unsigned nr_pages, char *msg)
{
	s64 elapsed_centisecs64;
	int centisecs;
	int k;
	int kps;

	elapsed_centisecs64 = timeval_to_ns(stop) - timeval_to_ns(start);
	do_div(elapsed_centisecs64, NSEC_PER_SEC / 100);
	centisecs = elapsed_centisecs64;
	if (centisecs == 0)
		centisecs = 1;	/* avoid div-by-zero */
	k = nr_pages * (PAGE_SIZE / 1024);
	kps = (k * 100) / centisecs;
	printk("%s %d kbytes in %d.%02d seconds (%d.%02d MB/s)\n", msg, k,
			centisecs / 100, centisecs % 100,
			kps / 1000, (kps % 1000) / 10);
}

static int get_swap_writer(struct swap_map_handle *handle)
{
	handle->cur = (struct swap_map_page *)get_zeroed_page(GFP_KERNEL);
	if (!handle->cur)
		return -ENOMEM;
	handle->bitmap = alloc_bitmap(count_swap_pages(root_swap, 0));
	if (!handle->bitmap) {
		release_swap_writer(handle);
		return -ENOMEM;
	}
	handle->cur_swap = alloc_swap_page(root_swap, handle->bitmap);
	if (!handle->cur_swap) {
		release_swap_writer(handle);
		return -ENOSPC;
	}
	handle->k = 0;
	return 0;
}

static int wait_on_bio_chain(struct bio **bio_chain)
{
	struct bio *bio;
	struct bio *next_bio;
	int ret = 0;

	if (bio_chain == NULL)
		return 0;

	bio = *bio_chain;
	if (bio == NULL)
		return 0;
	while (bio) {
		struct page *page;

		next_bio = bio->bi_private;
		page = bio->bi_io_vec[0].bv_page;
		wait_on_page_locked(page);
		if (!PageUptodate(page) || PageError(page))
			ret = -EIO;
		put_page(page);
		bio_put(bio);
		bio = next_bio;
	}
	*bio_chain = NULL;
	return ret;
}

static int swap_write_page(struct swap_map_handle *handle, void *buf,
				struct bio **bio_chain)
{
	int error = 0;
	unsigned long offset;

	if (!handle->cur)
		return -EINVAL;
	offset = alloc_swap_page(root_swap, handle->bitmap);
	error = write_page(buf, offset, bio_chain);
	if (error)
		return error;
	handle->cur->entries[handle->k++] = offset;
	if (handle->k >= MAP_PAGE_ENTRIES) {
		error = wait_on_bio_chain(bio_chain);
		if (error)
			goto out;
		offset = alloc_swap_page(root_swap, handle->bitmap);
		if (!offset)
			return -ENOSPC;
		handle->cur->next_swap = offset;
		error = write_page(handle->cur, handle->cur_swap, NULL);
		if (error)
			goto out;
		memset(handle->cur, 0, PAGE_SIZE);
		handle->cur_swap = offset;
		handle->k = 0;
	}
out:
	return error;
}

static int flush_swap_writer(struct swap_map_handle *handle)
{
	if (handle->cur && handle->cur_swap)
		return write_page(handle->cur, handle->cur_swap, NULL);
	else
		return -EINVAL;
}

/**
 *	save_image - save the suspend image data
 */

static int save_image(struct swap_map_handle *handle,
                      struct snapshot_handle *snapshot,
                      unsigned int nr_to_write)
{
	unsigned int m;
	int ret;
	int error = 0;
	int nr_pages;
	int err2;
	struct bio *bio;
	struct timeval start;
	struct timeval stop;

	printk("Saving image data pages (%u pages) ...     ", nr_to_write);
	m = nr_to_write / 100;
	if (!m)
		m = 1;
	nr_pages = 0;
	bio = NULL;
	do_gettimeofday(&start);
	do {
		ret = snapshot_read_next(snapshot, PAGE_SIZE);
		if (ret > 0) {
			error = swap_write_page(handle, data_of(*snapshot),
						&bio);
			if (error)
				break;
			if (!(nr_pages % m))
				printk("\b\b\b\b%3d%%", nr_pages / m);
			nr_pages++;
		}
	} while (ret > 0);
	err2 = wait_on_bio_chain(&bio);
	do_gettimeofday(&stop);
	if (!error)
		error = err2;
	if (!error)
		printk("\b\b\b\bdone\n");
	show_speed(&start, &stop, nr_to_write, "Wrote");
	return error;
}

/**
 *	enough_swap - Make sure we have enough swap to save the image.
 *
 *	Returns TRUE or FALSE after checking the total amount of swap
 *	space avaiable from the resume partition.
 */

static int enough_swap(unsigned int nr_pages)
{
	unsigned int free_swap = count_swap_pages(root_swap, 1);

	pr_debug("swsusp: free swap pages: %u\n", free_swap);
	return free_swap > nr_pages + PAGES_FOR_IO;
}

/**
 *	swsusp_write - Write entire image and metadata.
 *
 *	It is important _NOT_ to umount filesystems at this point. We want
 *	them synced (in case something goes wrong) but we DO not want to mark
 *	filesystem clean: it is not. (And it does not matter, if we resume
 *	correctly, we'll mark system clean, anyway.)
 */

int swsusp_write(void)
{
	struct swap_map_handle handle;
	struct snapshot_handle snapshot;
	struct swsusp_info *header;
	int error;

	if ((error = swsusp_swap_check())) {
		printk(KERN_ERR "swsusp: Cannot find swap device, try "
				"swapon -a.\n");
		return error;
	}
	memset(&snapshot, 0, sizeof(struct snapshot_handle));
	error = snapshot_read_next(&snapshot, PAGE_SIZE);
	if (error < PAGE_SIZE)
		return error < 0 ? error : -EFAULT;
	header = (struct swsusp_info *)data_of(snapshot);
	if (!enough_swap(header->pages)) {
		printk(KERN_ERR "swsusp: Not enough free swap\n");
		return -ENOSPC;
	}
	error = get_swap_writer(&handle);
	if (!error) {
		unsigned long start = handle.cur_swap;
		error = swap_write_page(&handle, header, NULL);
		if (!error)
			error = save_image(&handle, &snapshot,
					header->pages - 1);
		if (!error) {
			flush_swap_writer(&handle);
			printk("S");
			error = mark_swapfiles(swp_entry(root_swap, start));
			printk("|\n");
		}
	}
	if (error)
		free_all_swap_pages(root_swap, handle.bitmap);
	release_swap_writer(&handle);
	return error;
}

static struct block_device *resume_bdev;

/**
 *	submit - submit BIO request.
 *	@rw:	READ or WRITE.
 *	@off	physical offset of page.
 *	@page:	page we're reading or writing.
 *	@bio_chain: list of pending biod (for async reading)
 *
 *	Straight from the textbook - allocate and initialize the bio.
 *	If we're reading, make sure the page is marked as dirty.
 *	Then submit it and, if @bio_chain == NULL, wait.
 */
static int submit(int rw, pgoff_t page_off, struct page *page,
			struct bio **bio_chain)
{
	struct bio *bio;

	bio = bio_alloc(GFP_ATOMIC, 1);
	if (!bio)
		return -ENOMEM;
	bio->bi_sector = page_off * (PAGE_SIZE >> 9);
	bio->bi_bdev = resume_bdev;
	bio->bi_end_io = end_swap_bio_read;

	if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		printk("swsusp: ERROR: adding page to bio at %ld\n", page_off);
		bio_put(bio);
		return -EFAULT;
	}

	lock_page(page);
	bio_get(bio);

	if (bio_chain == NULL) {
		submit_bio(rw | (1 << BIO_RW_SYNC), bio);
		wait_on_page_locked(page);
		if (rw == READ)
			bio_set_pages_dirty(bio);
		bio_put(bio);
	} else {
		if (rw == READ)
			get_page(page);	/* These pages are freed later */
		bio->bi_private = *bio_chain;
		*bio_chain = bio;
		submit_bio(rw | (1 << BIO_RW_SYNC), bio);
	}
	return 0;
}

static int bio_read_page(pgoff_t page_off, void *addr, struct bio **bio_chain)
{
	return submit(READ, page_off, virt_to_page(addr), bio_chain);
}

static int bio_write_page(pgoff_t page_off, void *addr)
{
	return submit(WRITE, page_off, virt_to_page(addr), NULL);
}

/**
 *	The following functions allow us to read data using a swap map
 *	in a file-alike way
 */

static void release_swap_reader(struct swap_map_handle *handle)
{
	if (handle->cur)
		free_page((unsigned long)handle->cur);
	handle->cur = NULL;
}

static int get_swap_reader(struct swap_map_handle *handle,
                                      swp_entry_t start)
{
	int error;

	if (!swp_offset(start))
		return -EINVAL;
	handle->cur = (struct swap_map_page *)get_zeroed_page(GFP_ATOMIC);
	if (!handle->cur)
		return -ENOMEM;
	error = bio_read_page(swp_offset(start), handle->cur, NULL);
	if (error) {
		release_swap_reader(handle);
		return error;
	}
	handle->k = 0;
	return 0;
}

static int swap_read_page(struct swap_map_handle *handle, void *buf,
				struct bio **bio_chain)
{
	unsigned long offset;
	int error;

	if (!handle->cur)
		return -EINVAL;
	offset = handle->cur->entries[handle->k];
	if (!offset)
		return -EFAULT;
	error = bio_read_page(offset, buf, bio_chain);
	if (error)
		return error;
	if (++handle->k >= MAP_PAGE_ENTRIES) {
		error = wait_on_bio_chain(bio_chain);
		handle->k = 0;
		offset = handle->cur->next_swap;
		if (!offset)
			release_swap_reader(handle);
		else if (!error)
			error = bio_read_page(offset, handle->cur, NULL);
	}
	return error;
}

/**
 *	load_image - load the image using the swap map handle
 *	@handle and the snapshot handle @snapshot
 *	(assume there are @nr_pages pages to load)
 */

static int load_image(struct swap_map_handle *handle,
                      struct snapshot_handle *snapshot,
                      unsigned int nr_to_read)
{
	unsigned int m;
	int error = 0;
	struct timeval start;
	struct timeval stop;
	struct bio *bio;
	int err2;
	unsigned nr_pages;

	printk("Loading image data pages (%u pages) ...     ", nr_to_read);
	m = nr_to_read / 100;
	if (!m)
		m = 1;
	nr_pages = 0;
	bio = NULL;
	do_gettimeofday(&start);
	for ( ; ; ) {
		error = snapshot_write_next(snapshot, PAGE_SIZE);
		if (error <= 0)
			break;
		error = swap_read_page(handle, data_of(*snapshot), &bio);
		if (error)
			break;
		if (snapshot->sync_read)
			error = wait_on_bio_chain(&bio);
		if (error)
			break;
		if (!(nr_pages % m))
			printk("\b\b\b\b%3d%%", nr_pages / m);
		nr_pages++;
	}
	err2 = wait_on_bio_chain(&bio);
	do_gettimeofday(&stop);
	if (!error)
		error = err2;
	if (!error) {
		printk("\b\b\b\bdone\n");
		snapshot_free_unused_memory(snapshot);
		if (!snapshot_image_loaded(snapshot))
			error = -ENODATA;
	}
	show_speed(&start, &stop, nr_to_read, "Read");
	return error;
}

int swsusp_read(void)
{
	int error;
	struct swap_map_handle handle;
	struct snapshot_handle snapshot;
	struct swsusp_info *header;

	if (IS_ERR(resume_bdev)) {
		pr_debug("swsusp: block device not initialised\n");
		return PTR_ERR(resume_bdev);
	}

	memset(&snapshot, 0, sizeof(struct snapshot_handle));
	error = snapshot_write_next(&snapshot, PAGE_SIZE);
	if (error < PAGE_SIZE)
		return error < 0 ? error : -EFAULT;
	header = (struct swsusp_info *)data_of(snapshot);
	error = get_swap_reader(&handle, swsusp_header.image);
	if (!error)
		error = swap_read_page(&handle, header, NULL);
	if (!error)
		error = load_image(&handle, &snapshot, header->pages - 1);
	release_swap_reader(&handle);

	blkdev_put(resume_bdev);

	if (!error)
		pr_debug("swsusp: Reading resume file was successful\n");
	else
		pr_debug("swsusp: Error %d resuming\n", error);
	return error;
}

/**
 *      swsusp_check - Check for swsusp signature in the resume device
 */

int swsusp_check(void)
{
	int error;

	resume_bdev = open_by_devnum(swsusp_resume_device, FMODE_READ);
	if (!IS_ERR(resume_bdev)) {
		set_blocksize(resume_bdev, PAGE_SIZE);
		memset(&swsusp_header, 0, sizeof(swsusp_header));
		if ((error = bio_read_page(0, &swsusp_header, NULL)))
			return error;
		if (!memcmp(SWSUSP_SIG, swsusp_header.sig, 10)) {
			memcpy(swsusp_header.sig, swsusp_header.orig_sig, 10);
			/* Reset swap signature now */
			error = bio_write_page(0, &swsusp_header);
		} else {
			return -EINVAL;
		}
		if (error)
			blkdev_put(resume_bdev);
		else
			pr_debug("swsusp: Signature found, resuming\n");
	} else {
		error = PTR_ERR(resume_bdev);
	}

	if (error)
		pr_debug("swsusp: Error %d check for resume file\n", error);

	return error;
}

/**
 *	swsusp_close - close swap device.
 */

void swsusp_close(void)
{
	if (IS_ERR(resume_bdev)) {
		pr_debug("swsusp: block device not initialised\n");
		return;
	}

	blkdev_put(resume_bdev);
}
