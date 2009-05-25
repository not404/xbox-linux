#ifndef _ASM_POWERPC_PAGE_H
#define _ASM_POWERPC_PAGE_H

/*
 * Copyright (C) 2001,2005 IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifdef __KERNEL__
#include <linux/config.h>
#include <asm/asm-compat.h>

/*
 * On PPC32 page size is 4K. For PPC64 we support either 4K or 64K software
 * page size. When using 64K pages however, whether we are really supporting
 * 64K pages in HW or not is irrelevant to those definitions.
 */
#ifdef CONFIG_PPC_64K_PAGES
#define PAGE_SHIFT		16
#else
#define PAGE_SHIFT		12
#endif

#define PAGE_SIZE		(ASM_CONST(1) << PAGE_SHIFT)

/* We do define AT_SYSINFO_EHDR but don't use the gate mechanism */
#define __HAVE_ARCH_GATE_AREA		1

/*
 * Subtle: (1 << PAGE_SHIFT) is an int, not an unsigned long. So if we
 * assign PAGE_MASK to a larger type it gets extended the way we want
 * (i.e. with 1s in the high bits)
 */
#define PAGE_MASK      (~((1 << PAGE_SHIFT) - 1))

#define PAGE_OFFSET     ASM_CONST(CONFIG_KERNEL_START)
#define KERNELBASE      PAGE_OFFSET

#ifdef CONFIG_DISCONTIGMEM
#define page_to_pfn(page)	discontigmem_page_to_pfn(page)
#define pfn_to_page(pfn)	discontigmem_pfn_to_page(pfn)
#define pfn_valid(pfn)		discontigmem_pfn_valid(pfn)
#endif

#ifdef CONFIG_FLATMEM
#define pfn_to_page(pfn)	(mem_map + (pfn))
#define page_to_pfn(page)	((unsigned long)((page) - mem_map))
#define pfn_valid(pfn)		((pfn) < max_mapnr)
#endif

#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_kaddr(pfn)	__va((pfn) << PAGE_SHIFT)
#define virt_addr_valid(kaddr)	pfn_valid(__pa(kaddr) >> PAGE_SHIFT)

#define __va(x) ((void *)((unsigned long)(x) + KERNELBASE))
#define __pa(x) ((unsigned long)(x) - PAGE_OFFSET)

/*
 * Unfortunately the PLT is in the BSS in the PPC32 ELF ABI,
 * and needs to be executable.  This means the whole heap ends
 * up being executable.
 */
#define VM_DATA_DEFAULT_FLAGS32	(VM_READ | VM_WRITE | VM_EXEC | \
				 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#define VM_DATA_DEFAULT_FLAGS64	(VM_READ | VM_WRITE | \
				 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#ifdef __powerpc64__
#include <asm/page_64.h>
#else
#include <asm/page_32.h>
#endif

/* align addr on a size boundary - adjust address up/down if needed */
#define _ALIGN_UP(addr,size)	(((addr)+((size)-1))&(~((size)-1)))
#define _ALIGN_DOWN(addr,size)	((addr)&(~((size)-1)))

/* align addr on a size boundary - adjust address up if needed */
#define _ALIGN(addr,size)     _ALIGN_UP(addr,size)

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	_ALIGN(addr, PAGE_SIZE)

#ifndef __ASSEMBLY__

#undef STRICT_MM_TYPECHECKS

#ifdef STRICT_MM_TYPECHECKS
/* These are used to make use of C type-checking. */

/* PTE level */
typedef struct { pte_basic_t pte; } pte_t;
#define pte_val(x)	((x).pte)
#define __pte(x)	((pte_t) { (x) })

/* 64k pages additionally define a bigger "real PTE" type that gathers
 * the "second half" part of the PTE for pseudo 64k pages
 */
#ifdef CONFIG_PPC_64K_PAGES
typedef struct { pte_t pte; unsigned long hidx; } real_pte_t;
#else
typedef struct { pte_t pte; } real_pte_t;
#endif

/* PMD level */
typedef struct { unsigned long pmd; } pmd_t;
#define pmd_val(x)	((x).pmd)
#define __pmd(x)	((pmd_t) { (x) })

/* PUD level exusts only on 4k pages */
#ifndef CONFIG_PPC_64K_PAGES
typedef struct { unsigned long pud; } pud_t;
#define pud_val(x)	((x).pud)
#define __pud(x)	((pud_t) { (x) })
#endif

/* PGD level */
typedef struct { unsigned long pgd; } pgd_t;
#define pgd_val(x)	((x).pgd)
#define __pgd(x)	((pgd_t) { (x) })

/* Page protection bits */
typedef struct { unsigned long pgprot; } pgprot_t;
#define pgprot_val(x)	((x).pgprot)
#define __pgprot(x)	((pgprot_t) { (x) })

#else

/*
 * .. while these make it easier on the compiler
 */

typedef pte_basic_t pte_t;
#define pte_val(x)	(x)
#define __pte(x)	(x)

#ifdef CONFIG_PPC_64K_PAGES
typedef struct { pte_t pte; unsigned long hidx; } real_pte_t;
#else
typedef unsigned long real_pte_t;
#endif


typedef unsigned long pmd_t;
#define pmd_val(x)	(x)
#define __pmd(x)	(x)

#ifndef CONFIG_PPC_64K_PAGES
typedef unsigned long pud_t;
#define pud_val(x)	(x)
#define __pud(x)	(x)
#endif

typedef unsigned long pgd_t;
#define pgd_val(x)	(x)
#define pgprot_val(x)	(x)

typedef unsigned long pgprot_t;
#define __pgd(x)	(x)
#define __pgprot(x)	(x)

#endif

struct page;
extern void clear_user_page(void *page, unsigned long vaddr, struct page *pg);
extern void copy_user_page(void *to, void *from, unsigned long vaddr,
		struct page *p);
extern int page_is_ram(unsigned long pfn);

#endif /* __ASSEMBLY__ */

#endif /* __KERNEL__ */

#endif /* _ASM_POWERPC_PAGE_H */
