/*
 *  Created 20 Oct 2004 by Mark Lord
 *
 *  Basic support for Delkin/ASKA/Workbit Cardbus CompactFlash adapter
 *
 *  Modeled after the 16-bit PCMCIA driver: ide-cs.c
 *
 *  This is slightly peculiar, in that it is a PCI driver,
 *  but is NOT an IDE PCI driver -- the IDE layer does not directly
 *  support hot insertion/removal of PCI interfaces, so this driver
 *  is unable to use the IDE PCI interfaces.  Instead, it uses the
 *  same interfaces as the ide-cs (PCMCIA) driver uses.
 *  On the plus side, the driver is also smaller/simpler this way.
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/hdreg.h>
#include <linux/ide.h>
#include <linux/init.h>
#include <linux/pci.h>

#include <asm/io.h>

/*
 * No chip documentation has yet been found,
 * so these configuration values were pulled from
 * a running Win98 system using "debug".
 * This gives around 3MByte/second read performance,
 * which is about 2/3 of what the chip is capable of.
 *
 * There is also a 4KByte mmio region on the card,
 * but its purpose has yet to be reverse-engineered.
 */
static const u8 setup[] = {
	0x00, 0x05, 0xbe, 0x01, 0x20, 0x8f, 0x00, 0x00,
	0xa4, 0x1f, 0xb3, 0x1b, 0x00, 0x00, 0x00, 0x80,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xa4, 0x83, 0x02, 0x13,
};

static int __devinit
delkin_cb_probe (struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned long base;
	hw_regs_t hw;
	ide_hwif_t *hwif = NULL;
	ide_drive_t *drive;
	int i, rc;
	u8 idx[4] = { 0xff, 0xff, 0xff, 0xff };

	rc = pci_enable_device(dev);
	if (rc) {
		printk(KERN_ERR "delkin_cb: pci_enable_device failed (%d)\n", rc);
		return rc;
	}
	rc = pci_request_regions(dev, "delkin_cb");
	if (rc) {
		printk(KERN_ERR "delkin_cb: pci_request_regions failed (%d)\n", rc);
		pci_disable_device(dev);
		return rc;
	}
	base = pci_resource_start(dev, 0);
	outb(0x02, base + 0x1e);	/* set nIEN to block interrupts */
	inb(base + 0x17);		/* read status to clear interrupts */
	for (i = 0; i < sizeof(setup); ++i) {
		if (setup[i])
			outb(setup[i], base + i);
	}
	pci_release_regions(dev);	/* IDE layer handles regions itself */

	memset(&hw, 0, sizeof(hw));
	ide_std_init_ports(&hw, base + 0x10, base + 0x1e);
	hw.irq = dev->irq;
	hw.chipset = ide_pci;		/* this enables IRQ sharing */

	hwif = ide_deprecated_find_port(hw.io_ports[IDE_DATA_OFFSET]);
	if (hwif == NULL)
		goto out_disable;

	i = hwif->index;

	if (hwif->present)
		ide_unregister(i, 0, 0);
	else if (!hwif->hold)
		ide_init_port_data(hwif, i);

	ide_init_port_hw(hwif, &hw);
	hwif->quirkproc = &ide_undecoded_slave;

	idx[0] = i;

	ide_device_add(idx, NULL);

	if (!hwif->present)
		goto out_disable;

	pci_set_drvdata(dev, hwif);
	hwif->dev = &dev->dev;
	drive = &hwif->drives[0];
	if (drive->present) {
		drive->io_32bit = 1;
		drive->unmask   = 1;
	}
	return 0;

out_disable:
	printk(KERN_ERR "delkin_cb: no IDE devices found\n");
	pci_disable_device(dev);
	return -ENODEV;
}

static void
delkin_cb_remove (struct pci_dev *dev)
{
	ide_hwif_t *hwif = pci_get_drvdata(dev);

	if (hwif)
		ide_unregister(hwif->index, 0, 0);

	pci_disable_device(dev);
}

static struct pci_device_id delkin_cb_pci_tbl[] __devinitdata = {
	{ 0x1145, 0xf021, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ 0x1145, 0xf024, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, delkin_cb_pci_tbl);

static struct pci_driver driver = {
	.name		= "Delkin-ASKA-Workbit Cardbus IDE",
	.id_table	= delkin_cb_pci_tbl,
	.probe		= delkin_cb_probe,
	.remove		= delkin_cb_remove,
};

static int
delkin_cb_init (void)
{
	return pci_register_driver(&driver);
}

static void
delkin_cb_exit (void)
{
	pci_unregister_driver(&driver);
}

module_init(delkin_cb_init);
module_exit(delkin_cb_exit);

MODULE_AUTHOR("Mark Lord");
MODULE_DESCRIPTION("Basic support for Delkin/ASKA/Workbit Cardbus IDE");
MODULE_LICENSE("GPL");

