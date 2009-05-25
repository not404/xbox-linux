/*
 * Copyright (C) 2001 Mike Corrigan & Dave Engebretsen, IBM Corporation
 * Rewrite, cleanup:
 * Copyright (C) 2004 Olof Johansson <olof@lixom.net>, IBM Corporation
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

#ifndef _ASM_IOMMU_H
#define _ASM_IOMMU_H

#include <linux/config.h>
#include <asm/types.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>

/*
 * IOMAP_MAX_ORDER defines the largest contiguous block
 * of dma space we can get.  IOMAP_MAX_ORDER = 13
 * allows up to 2**12 pages (4096 * 4096) = 16 MB
 */
#define IOMAP_MAX_ORDER 13

struct iommu_table {
	unsigned long  it_busno;     /* Bus number this table belongs to */
	unsigned long  it_size;      /* Size of iommu table in entries */
	unsigned long  it_offset;    /* Offset into global table */
	unsigned long  it_base;      /* mapped address of tce table */
	unsigned long  it_index;     /* which iommu table this is */
	unsigned long  it_type;      /* type: PCI or Virtual Bus */
	unsigned long  it_blocksize; /* Entries in each block (cacheline) */
	unsigned long  it_hint;      /* Hint for next alloc */
	unsigned long  it_largehint; /* Hint for large allocs */
	unsigned long  it_halfpoint; /* Breaking point for small/large allocs */
	spinlock_t     it_lock;      /* Protects it_map */
	unsigned long *it_map;       /* A simple allocation bitmap for now */
};

struct scatterlist;
struct device_node;

#ifdef CONFIG_PPC_MULTIPLATFORM

/* Walks all buses and creates iommu tables */
extern void iommu_setup_pSeries(void);
extern void iommu_setup_u3(void);

/* Frees table for an individual device node */
extern void iommu_free_table(struct device_node *dn);

#endif /* CONFIG_PPC_MULTIPLATFORM */

#ifdef CONFIG_PPC_PSERIES

/* Creates table for an individual device node */
extern void iommu_devnode_init_pSeries(struct device_node *dn);

#endif /* CONFIG_PPC_PSERIES */

#ifdef CONFIG_PPC_ISERIES

/* Creates table for an individual device node */
extern void iommu_devnode_init_iSeries(struct device_node *dn);
/* Get table parameters from HV */
extern void iommu_table_getparms_iSeries(unsigned long busno,
					 unsigned char slotno,
					 unsigned char virtbus,
					 struct iommu_table* tbl);

#endif /* CONFIG_PPC_ISERIES */

/* Initializes an iommu_table based in values set in the passed-in
 * structure
 */
extern struct iommu_table *iommu_init_table(struct iommu_table * tbl);

extern int iommu_map_sg(struct device *dev, struct iommu_table *tbl,
		struct scatterlist *sglist, int nelems,
		enum dma_data_direction direction);
extern void iommu_unmap_sg(struct iommu_table *tbl, struct scatterlist *sglist,
		int nelems, enum dma_data_direction direction);

extern void *iommu_alloc_coherent(struct iommu_table *tbl, size_t size,
		dma_addr_t *dma_handle, gfp_t flag);
extern void iommu_free_coherent(struct iommu_table *tbl, size_t size,
		void *vaddr, dma_addr_t dma_handle);
extern dma_addr_t iommu_map_single(struct iommu_table *tbl, void *vaddr,
		size_t size, enum dma_data_direction direction);
extern void iommu_unmap_single(struct iommu_table *tbl, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction direction);

extern void iommu_init_early_pSeries(void);
extern void iommu_init_early_iSeries(void);
extern void iommu_init_early_u3(void);

#ifdef CONFIG_PCI
extern void pci_iommu_init(void);
extern void pci_direct_iommu_init(void);
#else
static inline void pci_iommu_init(void) { }
#endif

extern void alloc_u3_dart_table(void);

#endif /* _ASM_IOMMU_H */
