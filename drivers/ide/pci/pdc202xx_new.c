/*
 *  Promise TX2/TX4/TX2000/133 IDE driver
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 *  Split from:
 *  linux/drivers/ide/pdc202xx.c	Version 0.35	Mar. 30, 2002
 *  Copyright (C) 1998-2002		Andre Hedrick <andre@linux-ide.org>
 *  Copyright (C) 2005-2006		MontaVista Software, Inc.
 *  Portions Copyright (C) 1999 Promise Technology, Inc.
 *  Author: Frank Tiernan (frankt@promise.com)
 *  Released under terms of General Public License
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/ide.h>

#include <asm/io.h>
#include <asm/irq.h>

#ifdef CONFIG_PPC_PMAC
#include <asm/prom.h>
#include <asm/pci-bridge.h>
#endif

#define PDC202_DEBUG_CABLE	0

#undef DEBUG

#ifdef DEBUG
#define DBG(fmt, args...) printk("%s: " fmt, __FUNCTION__, ## args)
#else
#define DBG(fmt, args...)
#endif

static const char *pdc_quirk_drives[] = {
	"QUANTUM FIREBALLlct08 08",
	"QUANTUM FIREBALLP KA6.4",
	"QUANTUM FIREBALLP KA9.1",
	"QUANTUM FIREBALLP LM20.4",
	"QUANTUM FIREBALLP KX13.6",
	"QUANTUM FIREBALLP KX20.5",
	"QUANTUM FIREBALLP KX27.3",
	"QUANTUM FIREBALLP LM20.5",
	NULL
};

static u8 max_dma_rate(struct pci_dev *pdev)
{
	u8 mode;

	switch(pdev->device) {
		case PCI_DEVICE_ID_PROMISE_20277:
		case PCI_DEVICE_ID_PROMISE_20276:
		case PCI_DEVICE_ID_PROMISE_20275:
		case PCI_DEVICE_ID_PROMISE_20271:
		case PCI_DEVICE_ID_PROMISE_20269:
			mode = 4;
			break;
		case PCI_DEVICE_ID_PROMISE_20270:
		case PCI_DEVICE_ID_PROMISE_20268:
			mode = 3;
			break;
		default:
			return 0;
	}

	return mode;
}

static u8 pdcnew_ratemask(ide_drive_t *drive)
{
	u8 mode = max_dma_rate(HWIF(drive)->pci_dev);

	if (!eighty_ninty_three(drive))
		mode = min_t(u8, mode, 1);

	return	mode;
}

static int check_in_drive_lists(ide_drive_t *drive, const char **list)
{
	struct hd_driveid *id = drive->id;

	if (pdc_quirk_drives == list) {
		while (*list) {
			if (strstr(id->model, *list++)) {
				return 2;
			}
		}
	} else {
		while (*list) {
			if (!strcmp(*list++,id->model)) {
				return 1;
			}
		}
	}
	return 0;
}

/**
 * get_indexed_reg - Get indexed register
 * @hwif: for the port address
 * @index: index of the indexed register
 */
static u8 get_indexed_reg(ide_hwif_t *hwif, u8 index)
{
	u8 value;

	hwif->OUTB(index, hwif->dma_vendor1);
	value = hwif->INB(hwif->dma_vendor3);

	DBG("index[%02X] value[%02X]\n", index, value);
	return value;
}

/**
 * set_indexed_reg - Set indexed register
 * @hwif: for the port address
 * @index: index of the indexed register
 */
static void set_indexed_reg(ide_hwif_t *hwif, u8 index, u8 value)
{
	hwif->OUTB(index, hwif->dma_vendor1);
	hwif->OUTB(value, hwif->dma_vendor3);
	DBG("index[%02X] value[%02X]\n", index, value);
}

/*
 * ATA Timing Tables based on 133 MHz PLL output clock.
 *
 * If the PLL outputs 100 MHz clock, the ASIC hardware will set
 * the timing registers automatically when "set features" command is
 * issued to the device. However, if the PLL output clock is 133 MHz,
 * the following tables must be used.
 */
static struct pio_timing {
	u8 reg0c, reg0d, reg13;
} pio_timings [] = {
	{ 0xfb, 0x2b, 0xac },	/* PIO mode 0, IORDY off, Prefetch off */
	{ 0x46, 0x29, 0xa4 },	/* PIO mode 1, IORDY off, Prefetch off */
	{ 0x23, 0x26, 0x64 },	/* PIO mode 2, IORDY off, Prefetch off */
	{ 0x27, 0x0d, 0x35 },	/* PIO mode 3, IORDY on,  Prefetch off */
	{ 0x23, 0x09, 0x25 },	/* PIO mode 4, IORDY on,  Prefetch off */
};

static struct mwdma_timing {
	u8 reg0e, reg0f;
} mwdma_timings [] = {
	{ 0xdf, 0x5f }, 	/* MWDMA mode 0 */
	{ 0x6b, 0x27 }, 	/* MWDMA mode 1 */
	{ 0x69, 0x25 }, 	/* MWDMA mode 2 */
};

static struct udma_timing {
	u8 reg10, reg11, reg12;
} udma_timings [] = {
	{ 0x4a, 0x0f, 0xd5 },	/* UDMA mode 0 */
	{ 0x3a, 0x0a, 0xd0 },	/* UDMA mode 1 */
	{ 0x2a, 0x07, 0xcd },	/* UDMA mode 2 */
	{ 0x1a, 0x05, 0xcd },	/* UDMA mode 3 */
	{ 0x1a, 0x03, 0xcd },	/* UDMA mode 4 */
	{ 0x1a, 0x02, 0xcb },	/* UDMA mode 5 */
	{ 0x1a, 0x01, 0xcb },	/* UDMA mode 6 */
};

static int pdcnew_tune_chipset(ide_drive_t *drive, u8 speed)
{
	ide_hwif_t *hwif	= HWIF(drive);
	u8 adj			= (drive->dn & 1) ? 0x08 : 0x00;
	int			err;

	speed = ide_rate_filter(pdcnew_ratemask(drive), speed);

	/*
	 * Issue SETFEATURES_XFER to the drive first. PDC202xx hardware will
	 * automatically set the timing registers based on 100 MHz PLL output.
	 */
 	err = ide_config_drive_speed(drive, speed);

	/*
	 * As we set up the PLL to output 133 MHz for UltraDMA/133 capable
	 * chips, we must override the default register settings...
	 */
	if (max_dma_rate(hwif->pci_dev) == 4) {
		u8 mode = speed & 0x07;

		switch (speed) {
			case XFER_UDMA_6:
			case XFER_UDMA_5:
			case XFER_UDMA_4:
			case XFER_UDMA_3:
			case XFER_UDMA_2:
			case XFER_UDMA_1:
			case XFER_UDMA_0:
				set_indexed_reg(hwif, 0x10 + adj,
						udma_timings[mode].reg10);
				set_indexed_reg(hwif, 0x11 + adj,
						udma_timings[mode].reg11);
				set_indexed_reg(hwif, 0x12 + adj,
						udma_timings[mode].reg12);
				break;

			case XFER_MW_DMA_2:
			case XFER_MW_DMA_1:
			case XFER_MW_DMA_0:
				set_indexed_reg(hwif, 0x0e + adj,
						mwdma_timings[mode].reg0e);
				set_indexed_reg(hwif, 0x0f + adj,
						mwdma_timings[mode].reg0f);
				break;
			case XFER_PIO_4:
			case XFER_PIO_3:
			case XFER_PIO_2:
			case XFER_PIO_1:
			case XFER_PIO_0:
				set_indexed_reg(hwif, 0x0c + adj,
						pio_timings[mode].reg0c);
				set_indexed_reg(hwif, 0x0d + adj,
						pio_timings[mode].reg0d);
				set_indexed_reg(hwif, 0x13 + adj,
						pio_timings[mode].reg13);
				break;
			default:
				printk(KERN_ERR "pdc202xx_new: "
				       "Unknown speed %d ignored\n", speed);
		}
	} else if (speed == XFER_UDMA_2) {
		/* Set tHOLD bit to 0 if using UDMA mode 2 */
		u8 tmp = get_indexed_reg(hwif, 0x10 + adj);

		set_indexed_reg(hwif, 0x10 + adj, tmp & 0x7f);
 	}

	return err;
}

/*   0    1    2    3    4    5    6   7   8
 * 960, 480, 390, 300, 240, 180, 120, 90, 60
 *           180, 150, 120,  90,  60
 * DMA_Speed
 * 180, 120,  90,  90,  90,  60,  30
 *  11,   5,   4,   3,   2,   1,   0
 */
static void pdcnew_tune_drive(ide_drive_t *drive, u8 pio)
{
	pio = ide_get_best_pio_mode(drive, pio, 4, NULL);
	(void)pdcnew_tune_chipset(drive, XFER_PIO_0 + pio);
}

static u8 pdcnew_cable_detect(ide_hwif_t *hwif)
{
	return get_indexed_reg(hwif, 0x0b) & 0x04;
}

static int config_chipset_for_dma(ide_drive_t *drive)
{
	struct hd_driveid *id	= drive->id;
	ide_hwif_t *hwif	= HWIF(drive);
	u8 ultra_66		= (id->dma_ultra & 0x0078) ? 1 : 0;
	u8 cable		= pdcnew_cable_detect(hwif);
	u8 speed;

	if (ultra_66 && cable) {
		printk(KERN_WARNING "Warning: %s channel "
		       "requires an 80-pin cable for operation.\n",
		       hwif->channel ? "Secondary" : "Primary");
		printk(KERN_WARNING "%s reduced to Ultra33 mode.\n", drive->name);
	}

	if (drive->media != ide_disk)
		return 0;

	if (id->capability & 4) {
		/*
		 * Set IORDY_EN & PREFETCH_EN (this seems to have
		 * NO real effect since this register is reloaded
		 * by hardware when the transfer mode is selected)
		 */
		u8 tmp, adj = (drive->dn & 1) ? 0x08 : 0x00;

		tmp = get_indexed_reg(hwif, 0x13 + adj);
		set_indexed_reg(hwif, 0x13 + adj, tmp | 0x03);
	}

	speed = ide_dma_speed(drive, pdcnew_ratemask(drive));

	if (!speed)
		return 0;

	(void) hwif->speedproc(drive, speed);
	return ide_dma_enable(drive);
}

static int pdcnew_config_drive_xfer_rate(ide_drive_t *drive)
{
	ide_hwif_t *hwif	= HWIF(drive);
	struct hd_driveid *id	= drive->id;

	drive->init_speed = 0;

	if (id && (id->capability & 1) && drive->autodma) {

		if (ide_use_dma(drive)) {
			if (config_chipset_for_dma(drive))
				return hwif->ide_dma_on(drive);
		}

		goto fast_ata_pio;

	} else if ((id->capability & 8) || (id->field_valid & 2)) {
fast_ata_pio:
		hwif->tuneproc(drive, 255);
		return hwif->ide_dma_off_quietly(drive);
	}
	/* IORDY not supported */
	return 0;
}

static int pdcnew_quirkproc(ide_drive_t *drive)
{
	return check_in_drive_lists(drive, pdc_quirk_drives);
}

static int pdcnew_ide_dma_lostirq(ide_drive_t *drive)
{
	if (HWIF(drive)->resetproc != NULL)
		HWIF(drive)->resetproc(drive);
	return __ide_dma_lostirq(drive);
}

static int pdcnew_ide_dma_timeout(ide_drive_t *drive)
{
	if (HWIF(drive)->resetproc != NULL)
		HWIF(drive)->resetproc(drive);
	return __ide_dma_timeout(drive);
}

static void pdcnew_reset(ide_drive_t *drive)
{
	/*
	 * Deleted this because it is redundant from the caller.
	 */
	printk(KERN_WARNING "pdc202xx_new: %s channel reset.\n",
		HWIF(drive)->channel ? "Secondary" : "Primary");
}

/**
 * read_counter - Read the byte count registers
 * @dma_base: for the port address
 */
static long __devinit read_counter(u32 dma_base)
{
	u32  pri_dma_base = dma_base, sec_dma_base = dma_base + 0x08;
	u8   cnt0, cnt1, cnt2, cnt3;
	long count = 0, last;
	int  retry = 3;

	do {
		last = count;

		/* Read the current count */
		outb(0x20, pri_dma_base + 0x01);
		cnt0 = inb(pri_dma_base + 0x03);
		outb(0x21, pri_dma_base + 0x01);
		cnt1 = inb(pri_dma_base + 0x03);
		outb(0x20, sec_dma_base + 0x01);
		cnt2 = inb(sec_dma_base + 0x03);
		outb(0x21, sec_dma_base + 0x01);
		cnt3 = inb(sec_dma_base + 0x03);

		count = (cnt3 << 23) | (cnt2 << 15) | (cnt1 << 8) | cnt0;

		/*
		 * The 30-bit decrementing counter is read in 4 pieces.
		 * Incorrect value may be read when the most significant bytes
		 * are changing...
		 */
	} while (retry-- && (((last ^ count) & 0x3fff8000) || last < count));

	DBG("cnt0[%02X] cnt1[%02X] cnt2[%02X] cnt3[%02X]\n",
		  cnt0, cnt1, cnt2, cnt3);

	return count;
}

/**
 * detect_pll_input_clock - Detect the PLL input clock in Hz.
 * @dma_base: for the port address
 * E.g. 16949000 on 33 MHz PCI bus, i.e. half of the PCI clock.
 */
static long __devinit detect_pll_input_clock(unsigned long dma_base)
{
	long start_count, end_count;
	long pll_input;
	u8 scr1;

	start_count = read_counter(dma_base);

	/* Start the test mode */
	outb(0x01, dma_base + 0x01);
	scr1 = inb(dma_base + 0x03);
	DBG("scr1[%02X]\n", scr1);
	outb(scr1 | 0x40, dma_base + 0x03);

	/* Let the counter run for 10 ms. */
	mdelay(10);

	end_count = read_counter(dma_base);

	/* Stop the test mode */
	outb(0x01, dma_base + 0x01);
	scr1 = inb(dma_base + 0x03);
	DBG("scr1[%02X]\n", scr1);
	outb(scr1 & ~0x40, dma_base + 0x03);

	/*
	 * Calculate the input clock in Hz
	 * (the clock counter is 30 bit wide and counts down)
	 */
	pll_input = ((start_count - end_count) & 0x3ffffff) * 100;

	DBG("start[%ld] end[%ld]\n", start_count, end_count);

	return pll_input;
}

#ifdef CONFIG_PPC_PMAC
static void __devinit apple_kiwi_init(struct pci_dev *pdev)
{
	struct device_node *np = pci_device_to_OF_node(pdev);
	unsigned int class_rev = 0;
	u8 conf;

	if (np == NULL || !device_is_compatible(np, "kiwi-root"))
		return;

	pci_read_config_dword(pdev, PCI_CLASS_REVISION, &class_rev);
	class_rev &= 0xff;

	if (class_rev >= 0x03) {
		/* Setup chip magic config stuff (from darwin) */
		pci_read_config_byte (pdev, 0x40, &conf);
		pci_write_config_byte(pdev, 0x40, (conf | 0x01));
	}
}
#endif /* CONFIG_PPC_PMAC */

static unsigned int __devinit init_chipset_pdcnew(struct pci_dev *dev, const char *name)
{
	unsigned long dma_base = pci_resource_start(dev, 4);
	unsigned long sec_dma_base = dma_base + 0x08;
	long pll_input, pll_output, ratio;
	int f, r;
	u8 pll_ctl0, pll_ctl1;

	if (dev->resource[PCI_ROM_RESOURCE].start) {
		pci_write_config_dword(dev, PCI_ROM_ADDRESS,
			dev->resource[PCI_ROM_RESOURCE].start | PCI_ROM_ADDRESS_ENABLE);
		printk(KERN_INFO "%s: ROM enabled at 0x%08lx\n", name,
			(unsigned long)dev->resource[PCI_ROM_RESOURCE].start);
	}

#ifdef CONFIG_PPC_PMAC
	apple_kiwi_init(dev);
#endif

	/* Calculate the required PLL output frequency */
	switch(max_dma_rate(dev)) {
		case 4: /* it's 133 MHz for Ultra133 chips */
			pll_output = 133333333;
			break;
		case 3: /* and  100 MHz for Ultra100 chips */
		default:
			pll_output = 100000000;
			break;
	}

	/*
	 * Detect PLL input clock.
	 * On some systems, where PCI bus is running at non-standard clock rate
	 * (e.g. 25 or 40 MHz), we have to adjust the cycle time.
	 * PDC20268 and newer chips employ PLL circuit to help correct timing
	 * registers setting.
	 */
	pll_input = detect_pll_input_clock(dma_base);
	printk("%s: PLL input clock is %ld kHz\n", name, pll_input / 1000);

	/* Sanity check */
	if (unlikely(pll_input < 5000000L || pll_input > 70000000L)) {
		printk(KERN_ERR "%s: Bad PLL input clock %ld Hz, giving up!\n",
		       name, pll_input);
		goto out;
	}

#ifdef DEBUG
	DBG("pll_output is %ld Hz\n", pll_output);

	/* Show the current clock value of PLL control register
	 * (maybe already configured by the BIOS)
	 */
	outb(0x02, sec_dma_base + 0x01);
	pll_ctl0 = inb(sec_dma_base + 0x03);
	outb(0x03, sec_dma_base + 0x01);
	pll_ctl1 = inb(sec_dma_base + 0x03);

	DBG("pll_ctl[%02X][%02X]\n", pll_ctl0, pll_ctl1);
#endif

	/*
	 * Calculate the ratio of F, R and NO
	 * POUT = (F + 2) / (( R + 2) * NO)
	 */
	ratio = pll_output / (pll_input / 1000);
	if (ratio < 8600L) { /* 8.6x */
		/* Using NO = 0x01, R = 0x0d */
		r = 0x0d;
	} else if (ratio < 12900L) { /* 12.9x */
		/* Using NO = 0x01, R = 0x08 */
		r = 0x08;
	} else if (ratio < 16100L) { /* 16.1x */
		/* Using NO = 0x01, R = 0x06 */
		r = 0x06;
	} else if (ratio < 64000L) { /* 64x */
		r = 0x00;
	} else {
		/* Invalid ratio */
		printk(KERN_ERR "%s: Bad ratio %ld, giving up!\n", name, ratio);
		goto out;
	}

	f = (ratio * (r + 2)) / 1000 - 2;

	DBG("F[%d] R[%d] ratio*1000[%ld]\n", f, r, ratio);

	if (unlikely(f < 0 || f > 127)) {
		/* Invalid F */
		printk(KERN_ERR "%s: F[%d] invalid!\n", name, f);
		goto out;
	}

	pll_ctl0 = (u8) f;
	pll_ctl1 = (u8) r;

	DBG("Writing pll_ctl[%02X][%02X]\n", pll_ctl0, pll_ctl1);

	outb(0x02,     sec_dma_base + 0x01);
	outb(pll_ctl0, sec_dma_base + 0x03);
	outb(0x03,     sec_dma_base + 0x01);
	outb(pll_ctl1, sec_dma_base + 0x03);

	/* Wait the PLL circuit to be stable */
	mdelay(30);

#ifdef DEBUG
	/*
	 *  Show the current clock value of PLL control register
	 */
	outb(0x02, sec_dma_base + 0x01);
	pll_ctl0 = inb(sec_dma_base + 0x03);
	outb(0x03, sec_dma_base + 0x01);
	pll_ctl1 = inb(sec_dma_base + 0x03);

	DBG("pll_ctl[%02X][%02X]\n", pll_ctl0, pll_ctl1);
#endif

 out:
	return dev->irq;
}

static void __devinit init_hwif_pdc202new(ide_hwif_t *hwif)
{
	hwif->autodma = 0;

	hwif->tuneproc  = &pdcnew_tune_drive;
	hwif->quirkproc = &pdcnew_quirkproc;
	hwif->speedproc = &pdcnew_tune_chipset;
	hwif->resetproc = &pdcnew_reset;

	hwif->drives[0].autotune = hwif->drives[1].autotune = 1;

	hwif->ultra_mask = 0x7f;
	hwif->mwdma_mask = 0x07;

	hwif->err_stops_fifo = 1;

	hwif->ide_dma_check = &pdcnew_config_drive_xfer_rate;
	hwif->ide_dma_lostirq = &pdcnew_ide_dma_lostirq;
	hwif->ide_dma_timeout = &pdcnew_ide_dma_timeout;

	if (!hwif->udma_four)
		hwif->udma_four = pdcnew_cable_detect(hwif) ? 0 : 1;

	if (!noautodma)
		hwif->autodma = 1;
	hwif->drives[0].autodma = hwif->drives[1].autodma = hwif->autodma;

#if PDC202_DEBUG_CABLE
	printk(KERN_DEBUG "%s: %s-pin cable\n",
		hwif->name, hwif->udma_four ? "80" : "40");
#endif /* PDC202_DEBUG_CABLE */
}

static int __devinit init_setup_pdcnew(struct pci_dev *dev, ide_pci_device_t *d)
{
	return ide_setup_pci_device(dev, d);
}

static int __devinit init_setup_pdc20270(struct pci_dev *dev,
					 ide_pci_device_t *d)
{
	struct pci_dev *findev = NULL;
	int ret;

	if ((dev->bus->self &&
	     dev->bus->self->vendor == PCI_VENDOR_ID_DEC) &&
	    (dev->bus->self->device == PCI_DEVICE_ID_DEC_21150)) {
		if (PCI_SLOT(dev->devfn) & 2)
			return -ENODEV;
		d->extra = 0;
		while ((findev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, findev)) != NULL) {
			if ((findev->vendor == dev->vendor) &&
			    (findev->device == dev->device) &&
			    (PCI_SLOT(findev->devfn) & 2)) {
				if (findev->irq != dev->irq) {
					findev->irq = dev->irq;
				}
				ret = ide_setup_pci_devices(dev, findev, d);
				pci_dev_put(findev);
				return ret;
			}
		}
	}
	return ide_setup_pci_device(dev, d);
}

static int __devinit init_setup_pdc20276(struct pci_dev *dev,
					 ide_pci_device_t *d)
{
	if ((dev->bus->self) &&
	    (dev->bus->self->vendor == PCI_VENDOR_ID_INTEL) &&
	    ((dev->bus->self->device == PCI_DEVICE_ID_INTEL_I960) ||
	     (dev->bus->self->device == PCI_DEVICE_ID_INTEL_I960RM))) {
		printk(KERN_INFO "ide: Skipping Promise PDC20276 "
			"attached to I2O RAID controller.\n");
		return -ENODEV;
	}
	return ide_setup_pci_device(dev, d);
}

static ide_pci_device_t pdcnew_chipsets[] __devinitdata = {
	{	/* 0 */
		.name		= "PDC20268",
		.init_setup	= init_setup_pdcnew,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 1 */
		.name		= "PDC20269",
		.init_setup	= init_setup_pdcnew,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 2 */
		.name		= "PDC20270",
		.init_setup	= init_setup_pdc20270,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 3 */
		.name		= "PDC20271",
		.init_setup	= init_setup_pdcnew,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 4 */
		.name		= "PDC20275",
		.init_setup	= init_setup_pdcnew,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 5 */
		.name		= "PDC20276",
		.init_setup	= init_setup_pdc20276,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	},{	/* 6 */
		.name		= "PDC20277",
		.init_setup	= init_setup_pdcnew,
		.init_chipset	= init_chipset_pdcnew,
		.init_hwif	= init_hwif_pdc202new,
		.channels	= 2,
		.autodma	= AUTODMA,
		.bootable	= OFF_BOARD,
	}
};

/**
 *	pdc202new_init_one	-	called when a pdc202xx is found
 *	@dev: the pdc202new device
 *	@id: the matching pci id
 *
 *	Called when the PCI registration layer (or the IDE initialization)
 *	finds a device matching our IDE device tables.
 */
 
static int __devinit pdc202new_init_one(struct pci_dev *dev, const struct pci_device_id *id)
{
	ide_pci_device_t *d = &pdcnew_chipsets[id->driver_data];

	return d->init_setup(dev, d);
}

static struct pci_device_id pdc202new_pci_tbl[] = {
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20268, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20269, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 1},
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20270, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 2},
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20271, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 3},
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20275, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 4},
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20276, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 5},
	{ PCI_VENDOR_ID_PROMISE, PCI_DEVICE_ID_PROMISE_20277, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 6},
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, pdc202new_pci_tbl);

static struct pci_driver driver = {
	.name		= "Promise_IDE",
	.id_table	= pdc202new_pci_tbl,
	.probe		= pdc202new_init_one,
};

static int __init pdc202new_ide_init(void)
{
	return ide_pci_register_driver(&driver);
}

module_init(pdc202new_ide_init);

MODULE_AUTHOR("Andre Hedrick, Frank Tiernan");
MODULE_DESCRIPTION("PCI driver module for Promise PDC20268 and higher");
MODULE_LICENSE("GPL");
