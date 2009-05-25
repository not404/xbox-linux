/*
 * Setup pointers to hardware-dependent routines.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1996, 97, 98, 2000, 03, 04, 06 Ralf Baechle (ralf@linux-mips.org)
 */
#include <linux/eisa.h>
#include <linux/hdreg.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mc146818rtc.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/console.h>
#include <linux/fb.h>
#include <linux/screen_info.h>

#ifdef CONFIG_ARC
#include <asm/arc/types.h>
#include <asm/sgialib.h>
#endif

#include <asm/bcache.h>
#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mc146818-time.h>
#include <asm/processor.h>
#include <asm/reboot.h>
#include <asm/sni.h>
#include <asm/time.h>
#include <asm/traps.h>

extern void sni_machine_restart(char *command);
extern void sni_machine_halt(void);
extern void sni_machine_power_off(void);

void __init plat_timer_setup(struct irqaction *irq)
{
	/* set the clock to 100 Hz */
	outb_p(0x34,0x43);		/* binary, mode 2, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	setup_irq(0, irq);
}

/*
 * A bit more gossip about the iron we're running on ...
 */
static inline void sni_pcimt_detect(void)
{
	char boardtype[80];
	unsigned char csmsr;
	char *p = boardtype;
	unsigned int asic;

	csmsr = *(volatile unsigned char *)PCIMT_CSMSR;

	p += sprintf(p, "%s PCI", (csmsr & 0x80) ? "RM200" : "RM300");
	if ((csmsr & 0x80) == 0)
		p += sprintf(p, ", board revision %s",
		             (csmsr & 0x20) ? "D" : "C");
	asic = csmsr & 0x80;
	asic = (csmsr & 0x08) ? asic : !asic;
	p += sprintf(p, ", ASIC PCI Rev %s", asic ? "1.0" : "1.1");
	printk("%s.\n", boardtype);
}

static void __init sni_display_setup(void)
{
#if defined(CONFIG_VT) && defined(CONFIG_VGA_CONSOLE) && defined(CONFIG_ARC)
	struct screen_info *si = &screen_info;
	DISPLAY_STATUS *di;

	di = ArcGetDisplayStatus(1);

	if (di) {
		si->orig_x		= di->CursorXPosition;
		si->orig_y		= di->CursorYPosition;
		si->orig_video_cols	= di->CursorMaxXPosition;
		si->orig_video_lines	= di->CursorMaxYPosition;
		si->orig_video_isVGA	= VIDEO_TYPE_VGAC;
		si->orig_video_points	= 16;
	}
#endif
}

static struct resource sni_io_resource = {
	.start	= 0x00001000UL,
	.end	= 0x03bfffffUL,
	.name	= "PCIMT IO MEM",
	.flags	= IORESOURCE_IO,
};

static struct resource pcimt_io_resources[] = {
	{
		.start	= 0x00,
		.end	= 0x1f,
		.name	= "dma1",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	=  0x40,
		.end	= 0x5f,
		.name	= "timer",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	=  0x60,
		.end	= 0x6f,
		.name	= "keyboard",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	=  0x80,
		.end	= 0x8f,
		.name	= "dma page reg",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	=  0xc0,
		.end	= 0xdf,
		.name	= "dma2",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	=  0xcfc,
		.end	= 0xcff,
		.name	= "PCI config data",
		.flags	= IORESOURCE_BUSY
	}
};

static struct resource sni_mem_resource = {
	.start	= 0x10000000UL,
	.end	= 0xffffffffUL,
	.name	= "PCIMT PCI MEM",
	.flags	= IORESOURCE_MEM
};

/*
 * The RM200/RM300 has a few holes in it's PCI/EISA memory address space used
 * for other purposes.  Be paranoid and allocate all of the before the PCI
 * code gets a chance to to map anything else there ...
 *
 * This leaves the following areas available:
 *
 * 0x10000000 - 0x1009ffff (640kB) PCI/EISA/ISA Bus Memory
 * 0x10100000 - 0x13ffffff ( 15MB) PCI/EISA/ISA Bus Memory
 * 0x18000000 - 0x1fbfffff (124MB) PCI/EISA Bus Memory
 * 0x1ff08000 - 0x1ffeffff (816kB) PCI/EISA Bus Memory
 * 0xa0000000 - 0xffffffff (1.5GB) PCI/EISA Bus Memory
 */
static struct resource pcimt_mem_resources[] = {
	{
		.start	= 0x100a0000,
		.end	= 0x100bffff,
		.name	= "Video RAM area",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x100c0000,
		.end	= 0x100fffff,
		.name	= "ISA Reserved",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x14000000,
		.end	= 0x17bfffff,
		.name	= "PCI IO",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x17c00000,
		.end	= 0x17ffffff,
		.name	= "Cache Replacement Area",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1a000000,
		.end	= 0x1a000003,
		.name	= "PCI INT Acknowledge",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1fc00000,
		.end	= 0x1fc7ffff,
		.name	= "Boot PROM",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1fc80000,
		.end	= 0x1fcfffff,
		.name	= "Diag PROM",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1fd00000,
		.end	= 0x1fdfffff,
		.name	= "X-Bus",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1fe00000,
		.end	= 0x1fefffff,
		.name	= "BIOS map",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1ff00000,
		.end	= 0x1ff7ffff,
		.name	= "NVRAM / EEPROM",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1fff0000,
		.end	= 0x1fffefff,
		.name	= "ASIC PCI",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x1ffff000,
		.end	= 0x1fffffff,
		.name	= "MP Agent",
		.flags	= IORESOURCE_BUSY
	}, {
		.start	= 0x20000000,
		.end	= 0x9fffffff,
		.name	= "Main Memory",
		.flags	= IORESOURCE_BUSY
	}
};

static void __init sni_resource_init(void)
{
	int i;

	/* request I/O space for devices used on all i[345]86 PCs */
	for (i = 0; i < ARRAY_SIZE(pcimt_io_resources); i++)
		request_resource(&ioport_resource, pcimt_io_resources + i);

	/* request mem space for pcimt-specific devices */
	for (i = 0; i < ARRAY_SIZE(pcimt_mem_resources); i++)
		request_resource(&sni_mem_resource, pcimt_mem_resources + i);

	ioport_resource.end = sni_io_resource.end;
}

extern struct pci_ops sni_pci_ops;

static struct pci_controller sni_controller = {
	.pci_ops	= &sni_pci_ops,
	.mem_resource	= &sni_mem_resource,
	.mem_offset	= 0x10000000UL,
	.io_resource	= &sni_io_resource,
	.io_offset	= 0x00000000UL
};

static inline void sni_pcimt_time_init(void)
{
	rtc_mips_get_time = mc146818_get_cmos_time;
	rtc_mips_set_time = mc146818_set_rtc_mmss;
}

void __init plat_mem_setup(void)
{
	sni_pcimt_detect();
	sni_pcimt_sc_init();
	sni_pcimt_time_init();

	set_io_port_base(SNI_PORT_BASE);
	ioport_resource.end = sni_io_resource.end;

	/*
	 * Setup (E)ISA I/O memory access stuff
	 */
	isa_slot_offset = 0xb0000000;
#ifdef CONFIG_EISA
	EISA_bus = 1;
#endif

	sni_resource_init();

	_machine_restart = sni_machine_restart;
	_machine_halt = sni_machine_halt;
	pm_power_off = sni_machine_power_off;

	sni_display_setup();

#ifdef CONFIG_PCI
	register_pci_controller(&sni_controller);
#endif
}
