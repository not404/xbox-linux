/*
 * arch/powerpc/sysdev/dart_iommu.c
 *
 * Copyright (C) 2004 Olof Johansson <olof@lixom.net>, IBM Corporation
 * Copyright (C) 2005 Benjamin Herrenschmidt <benh@kernel.crashing.org>,
 *                    IBM Corporation
 *
 * Based on pSeries_iommu.c:
 * Copyright (C) 2001 Mike Corrigan & Dave Engebretsen, IBM Corporation
 * Copyright (C) 2004 Olof Johansson <olof@lixom.net>, IBM Corporation
 *
 * Dynamic DMA mapping support, Apple U3, U4 & IBM CPC925 "DART" iommu.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/iommu.h>
#include <asm/pci-bridge.h>
#include <asm/machdep.h>
#include <asm/abs_addr.h>
#include <asm/cacheflush.h>
#include <asm/lmb.h>
#include <asm/ppc-pci.h>

#include "dart.h"

extern int iommu_is_off;
extern int iommu_force_on;

/* Physical base address and size of the DART table */
unsigned long dart_tablebase; /* exported to htab_initialize */
static unsigned long dart_tablesize;

/* Virtual base address of the DART table */
static u32 *dart_vbase;

/* Mapped base address for the dart */
static unsigned int __iomem *dart;

/* Dummy val that entries are set to when unused */
static unsigned int dart_emptyval;

static struct iommu_table iommu_table_dart;
static int iommu_table_dart_inited;
static int dart_dirty;
static int dart_is_u4;

#define DBG(...)

static inline void dart_tlb_invalidate_all(void)
{
	unsigned long l = 0;
	unsigned int reg, inv_bit;
	unsigned long limit;

	DBG("dart: flush\n");

	/* To invalidate the DART, set the DARTCNTL_FLUSHTLB bit in the
	 * control register and wait for it to clear.
	 *
	 * Gotcha: Sometimes, the DART won't detect that the bit gets
	 * set. If so, clear it and set it again.
	 */

	limit = 0;

	inv_bit = dart_is_u4 ? DART_CNTL_U4_FLUSHTLB : DART_CNTL_U3_FLUSHTLB;
retry:
	l = 0;
	reg = DART_IN(DART_CNTL);
	reg |= inv_bit;
	DART_OUT(DART_CNTL, reg);

	while ((DART_IN(DART_CNTL) & inv_bit) && l < (1L << limit))
		l++;
	if (l == (1L << limit)) {
		if (limit < 4) {
			limit++;
			reg = DART_IN(DART_CNTL);
			reg &= ~inv_bit;
			DART_OUT(DART_CNTL, reg);
			goto retry;
		} else
			panic("DART: TLB did not flush after waiting a long "
			      "time. Buggy U3 ?");
	}
}

static inline void dart_tlb_invalidate_one(unsigned long bus_rpn)
{
	unsigned int reg;
	unsigned int l, limit;

	reg = DART_CNTL_U4_ENABLE | DART_CNTL_U4_IONE |
		(bus_rpn & DART_CNTL_U4_IONE_MASK);
	DART_OUT(DART_CNTL, reg);

	limit = 0;
wait_more:
	l = 0;
	while ((DART_IN(DART_CNTL) & DART_CNTL_U4_IONE) && l < (1L << limit)) {
		rmb();
		l++;
	}

	if (l == (1L << limit)) {
		if (limit < 4) {
			limit++;
			goto wait_more;
		} else
			panic("DART: TLB did not flush after waiting a long "
			      "time. Buggy U4 ?");
	}
}

static void dart_flush(struct iommu_table *tbl)
{
	mb();
	if (dart_dirty) {
		dart_tlb_invalidate_all();
		dart_dirty = 0;
	}
}

static void dart_build(struct iommu_table *tbl, long index,
		       long npages, unsigned long uaddr,
		       enum dma_data_direction direction)
{
	unsigned int *dp;
	unsigned int rpn;
	long l;

	DBG("dart: build at: %lx, %lx, addr: %x\n", index, npages, uaddr);

	index <<= DART_PAGE_FACTOR;
	npages <<= DART_PAGE_FACTOR;

	dp = ((unsigned int*)tbl->it_base) + index;

	/* On U3, all memory is contigous, so we can move this
	 * out of the loop.
	 */
	l = npages;
	while (l--) {
		rpn = virt_to_abs(uaddr) >> DART_PAGE_SHIFT;

		*(dp++) = DARTMAP_VALID | (rpn & DARTMAP_RPNMASK);

		uaddr += DART_PAGE_SIZE;
	}

	/* make sure all updates have reached memory */
	mb();
	in_be32((unsigned __iomem *)dp);
	mb();

	if (dart_is_u4) {
		rpn = index;
		while (npages--)
			dart_tlb_invalidate_one(rpn++);
	} else {
		dart_dirty = 1;
	}
}


static void dart_free(struct iommu_table *tbl, long index, long npages)
{
	unsigned int *dp;

	/* We don't worry about flushing the TLB cache. The only drawback of
	 * not doing it is that we won't catch buggy device drivers doing
	 * bad DMAs, but then no 32-bit architecture ever does either.
	 */

	DBG("dart: free at: %lx, %lx\n", index, npages);

	index <<= DART_PAGE_FACTOR;
	npages <<= DART_PAGE_FACTOR;

	dp  = ((unsigned int *)tbl->it_base) + index;

	while (npages--)
		*(dp++) = dart_emptyval;
}


static int dart_init(struct device_node *dart_node)
{
	unsigned int i;
	unsigned long tmp, base, size;
	struct resource r;

	if (dart_tablebase == 0 || dart_tablesize == 0) {
		printk(KERN_INFO "DART: table not allocated, using "
		       "direct DMA\n");
		return -ENODEV;
	}

	if (of_address_to_resource(dart_node, 0, &r))
		panic("DART: can't get register base ! ");

	/* Make sure nothing from the DART range remains in the CPU cache
	 * from a previous mapping that existed before the kernel took
	 * over
	 */
	flush_dcache_phys_range(dart_tablebase,
				dart_tablebase + dart_tablesize);

	/* Allocate a spare page to map all invalid DART pages. We need to do
	 * that to work around what looks like a problem with the HT bridge
	 * prefetching into invalid pages and corrupting data
	 */
	tmp = lmb_alloc(DART_PAGE_SIZE, DART_PAGE_SIZE);
	dart_emptyval = DARTMAP_VALID | ((tmp >> DART_PAGE_SHIFT) &
					 DARTMAP_RPNMASK);

	/* Map in DART registers */
	dart = ioremap(r.start, r.end - r.start + 1);
	if (dart == NULL)
		panic("DART: Cannot map registers!");

	/* Map in DART table */
	dart_vbase = ioremap(virt_to_abs(dart_tablebase), dart_tablesize);

	/* Fill initial table */
	for (i = 0; i < dart_tablesize/4; i++)
		dart_vbase[i] = dart_emptyval;

	/* Initialize DART with table base and enable it. */
	base = dart_tablebase >> DART_PAGE_SHIFT;
	size = dart_tablesize >> DART_PAGE_SHIFT;
	if (dart_is_u4) {
		size &= DART_SIZE_U4_SIZE_MASK;
		DART_OUT(DART_BASE_U4, base);
		DART_OUT(DART_SIZE_U4, size);
		DART_OUT(DART_CNTL, DART_CNTL_U4_ENABLE);
	} else {
		size &= DART_CNTL_U3_SIZE_MASK;
		DART_OUT(DART_CNTL,
			 DART_CNTL_U3_ENABLE |
			 (base << DART_CNTL_U3_BASE_SHIFT) |
			 (size << DART_CNTL_U3_SIZE_SHIFT));
	}

	/* Invalidate DART to get rid of possible stale TLBs */
	dart_tlb_invalidate_all();

	printk(KERN_INFO "DART IOMMU initialized for %s type chipset\n",
	       dart_is_u4 ? "U4" : "U3");

	return 0;
}

static void iommu_table_dart_setup(void)
{
	iommu_table_dart.it_busno = 0;
	iommu_table_dart.it_offset = 0;
	/* it_size is in number of entries */
	iommu_table_dart.it_size = (dart_tablesize / sizeof(u32)) >> DART_PAGE_FACTOR;

	/* Initialize the common IOMMU code */
	iommu_table_dart.it_base = (unsigned long)dart_vbase;
	iommu_table_dart.it_index = 0;
	iommu_table_dart.it_blocksize = 1;
	iommu_init_table(&iommu_table_dart, -1);

	/* Reserve the last page of the DART to avoid possible prefetch
	 * past the DART mapped area
	 */
	set_bit(iommu_table_dart.it_size - 1, iommu_table_dart.it_map);
}

static void iommu_dev_setup_dart(struct pci_dev *dev)
{
	struct device_node *dn;

	/* We only have one iommu table on the mac for now, which makes
	 * things simple. Setup all PCI devices to point to this table
	 *
	 * We must use pci_device_to_OF_node() to make sure that
	 * we get the real "final" pointer to the device in the
	 * pci_dev sysdata and not the temporary PHB one
	 */
	dn = pci_device_to_OF_node(dev);

	if (dn)
		PCI_DN(dn)->iommu_table = &iommu_table_dart;
}

static void iommu_bus_setup_dart(struct pci_bus *bus)
{
	struct device_node *dn;

	if (!iommu_table_dart_inited) {
		iommu_table_dart_inited = 1;
		iommu_table_dart_setup();
	}

	dn = pci_bus_to_OF_node(bus);

	if (dn)
		PCI_DN(dn)->iommu_table = &iommu_table_dart;
}

static void iommu_dev_setup_null(struct pci_dev *dev) { }
static void iommu_bus_setup_null(struct pci_bus *bus) { }

void iommu_init_early_dart(void)
{
	struct device_node *dn;

	/* Find the DART in the device-tree */
	dn = of_find_compatible_node(NULL, "dart", "u3-dart");
	if (dn == NULL) {
		dn = of_find_compatible_node(NULL, "dart", "u4-dart");
		if (dn == NULL)
			goto bail;
		dart_is_u4 = 1;
	}

	/* Setup low level TCE operations for the core IOMMU code */
	ppc_md.tce_build = dart_build;
	ppc_md.tce_free  = dart_free;
	ppc_md.tce_flush = dart_flush;

	/* Initialize the DART HW */
	if (dart_init(dn) == 0) {
		ppc_md.iommu_dev_setup = iommu_dev_setup_dart;
		ppc_md.iommu_bus_setup = iommu_bus_setup_dart;

		/* Setup pci_dma ops */
		pci_iommu_init();

		return;
	}

 bail:
	/* If init failed, use direct iommu and null setup functions */
	ppc_md.iommu_dev_setup = iommu_dev_setup_null;
	ppc_md.iommu_bus_setup = iommu_bus_setup_null;

	/* Setup pci_dma ops */
	pci_direct_iommu_init();
}


void __init alloc_dart_table(void)
{
	/* Only reserve DART space if machine has more than 1GB of RAM
	 * or if requested with iommu=on on cmdline.
	 *
	 * 1GB of RAM is picked as limit because some default devices
	 * (i.e. Airport Extreme) have 30 bit address range limits.
	 */

	if (iommu_is_off)
		return;

	if (!iommu_force_on && lmb_end_of_DRAM() <= 0x40000000ull)
		return;

	/* 512 pages (2MB) is max DART tablesize. */
	dart_tablesize = 1UL << 21;
	/* 16MB (1 << 24) alignment. We allocate a full 16Mb chuck since we
	 * will blow up an entire large page anyway in the kernel mapping
	 */
	dart_tablebase = (unsigned long)
		abs_to_virt(lmb_alloc_base(1UL<<24, 1UL<<24, 0x80000000L));

	printk(KERN_INFO "DART table allocated at: %lx\n", dart_tablebase);
}
