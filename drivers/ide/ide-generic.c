/*
 * generic/default IDE host driver
 *
 * Copyright (C) 2004 Bartlomiej Zolnierkiewicz
 * This code was split off from ide.c.  See it for original copyrights.
 *
 * May be copied or modified under the terms of the GNU General Public License.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ide.h>

static int __init ide_generic_init(void)
{
	u8 idx[MAX_HWIFS];
	int i;

	for (i = 0; i < MAX_HWIFS; i++) {
		ide_hwif_t *hwif = &ide_hwifs[i];

		if (hwif->io_ports[IDE_DATA_OFFSET] && !hwif->present)
			idx[i] = i;
		else
			idx[i] = 0xff;
	}

	ide_device_add_all(idx, NULL);

	return 0;
}

module_init(ide_generic_init);

MODULE_LICENSE("GPL");
