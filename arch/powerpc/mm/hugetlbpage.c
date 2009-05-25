/*
 * PPC64 (POWER4) Huge TLB Page Support for Kernel.
 *
 * Copyright (C) 2003 David Gibson, IBM Corporation.
 *
 * Based on the IA-32 version:
 * Copyright (C) 2002, Rohit Seth <rohit.seth@intel.com>
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/sysctl.h>
#include <asm/mman.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/machdep.h>
#include <asm/cputable.h>
#include <asm/tlb.h>

#include <linux/sysctl.h>

#define NUM_LOW_AREAS	(0x100000000UL >> SID_SHIFT)
#define NUM_HIGH_AREAS	(PGTABLE_RANGE >> HTLB_AREA_SHIFT)

#ifdef CONFIG_PPC_64K_PAGES
#define HUGEPTE_INDEX_SIZE	(PMD_SHIFT-HPAGE_SHIFT)
#else
#define HUGEPTE_INDEX_SIZE	(PUD_SHIFT-HPAGE_SHIFT)
#endif
#define PTRS_PER_HUGEPTE	(1 << HUGEPTE_INDEX_SIZE)
#define HUGEPTE_TABLE_SIZE	(sizeof(pte_t) << HUGEPTE_INDEX_SIZE)

#define HUGEPD_SHIFT		(HPAGE_SHIFT + HUGEPTE_INDEX_SIZE)
#define HUGEPD_SIZE		(1UL << HUGEPD_SHIFT)
#define HUGEPD_MASK		(~(HUGEPD_SIZE-1))

#define huge_pgtable_cache	(pgtable_cache[HUGEPTE_CACHE_NUM])

/* Flag to mark huge PD pointers.  This means pmd_bad() and pud_bad()
 * will choke on pointers to hugepte tables, which is handy for
 * catching screwups early. */
#define HUGEPD_OK	0x1

typedef struct { unsigned long pd; } hugepd_t;

#define hugepd_none(hpd)	((hpd).pd == 0)

static inline pte_t *hugepd_page(hugepd_t hpd)
{
	BUG_ON(!(hpd.pd & HUGEPD_OK));
	return (pte_t *)(hpd.pd & ~HUGEPD_OK);
}

static inline pte_t *hugepte_offset(hugepd_t *hpdp, unsigned long addr)
{
	unsigned long idx = ((addr >> HPAGE_SHIFT) & (PTRS_PER_HUGEPTE-1));
	pte_t *dir = hugepd_page(*hpdp);

	return dir + idx;
}

static int __hugepte_alloc(struct mm_struct *mm, hugepd_t *hpdp,
			   unsigned long address)
{
	pte_t *new = kmem_cache_alloc(huge_pgtable_cache,
				      GFP_KERNEL|__GFP_REPEAT);

	if (! new)
		return -ENOMEM;

	spin_lock(&mm->page_table_lock);
	if (!hugepd_none(*hpdp))
		kmem_cache_free(huge_pgtable_cache, new);
	else
		hpdp->pd = (unsigned long)new | HUGEPD_OK;
	spin_unlock(&mm->page_table_lock);
	return 0;
}

/* Modelled after find_linux_pte() */
pte_t *huge_pte_offset(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pg;
	pud_t *pu;

	BUG_ON(! in_hugepage_area(mm->context, addr));

	addr &= HPAGE_MASK;

	pg = pgd_offset(mm, addr);
	if (!pgd_none(*pg)) {
		pu = pud_offset(pg, addr);
		if (!pud_none(*pu)) {
#ifdef CONFIG_PPC_64K_PAGES
			pmd_t *pm;
			pm = pmd_offset(pu, addr);
			if (!pmd_none(*pm))
				return hugepte_offset((hugepd_t *)pm, addr);
#else
			return hugepte_offset((hugepd_t *)pu, addr);
#endif
		}
	}

	return NULL;
}

pte_t *huge_pte_alloc(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pg;
	pud_t *pu;
	hugepd_t *hpdp = NULL;

	BUG_ON(! in_hugepage_area(mm->context, addr));

	addr &= HPAGE_MASK;

	pg = pgd_offset(mm, addr);
	pu = pud_alloc(mm, pg, addr);

	if (pu) {
#ifdef CONFIG_PPC_64K_PAGES
		pmd_t *pm;
		pm = pmd_alloc(mm, pu, addr);
		if (pm)
			hpdp = (hugepd_t *)pm;
#else
		hpdp = (hugepd_t *)pu;
#endif
	}

	if (! hpdp)
		return NULL;

	if (hugepd_none(*hpdp) && __hugepte_alloc(mm, hpdp, addr))
		return NULL;

	return hugepte_offset(hpdp, addr);
}

static void free_hugepte_range(struct mmu_gather *tlb, hugepd_t *hpdp)
{
	pte_t *hugepte = hugepd_page(*hpdp);

	hpdp->pd = 0;
	tlb->need_flush = 1;
	pgtable_free_tlb(tlb, pgtable_free_cache(hugepte, HUGEPTE_CACHE_NUM,
						 HUGEPTE_TABLE_SIZE-1));
}

#ifdef CONFIG_PPC_64K_PAGES
static void hugetlb_free_pmd_range(struct mmu_gather *tlb, pud_t *pud,
				   unsigned long addr, unsigned long end,
				   unsigned long floor, unsigned long ceiling)
{
	pmd_t *pmd;
	unsigned long next;
	unsigned long start;

	start = addr;
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd))
			continue;
		free_hugepte_range(tlb, (hugepd_t *)pmd);
	} while (pmd++, addr = next, addr != end);

	start &= PUD_MASK;
	if (start < floor)
		return;
	if (ceiling) {
		ceiling &= PUD_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		return;

	pmd = pmd_offset(pud, start);
	pud_clear(pud);
	pmd_free_tlb(tlb, pmd);
}
#endif

static void hugetlb_free_pud_range(struct mmu_gather *tlb, pgd_t *pgd,
				   unsigned long addr, unsigned long end,
				   unsigned long floor, unsigned long ceiling)
{
	pud_t *pud;
	unsigned long next;
	unsigned long start;

	start = addr;
	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
#ifdef CONFIG_PPC_64K_PAGES
		if (pud_none_or_clear_bad(pud))
			continue;
		hugetlb_free_pmd_range(tlb, pud, addr, next, floor, ceiling);
#else
		if (pud_none(*pud))
			continue;
		free_hugepte_range(tlb, (hugepd_t *)pud);
#endif
	} while (pud++, addr = next, addr != end);

	start &= PGDIR_MASK;
	if (start < floor)
		return;
	if (ceiling) {
		ceiling &= PGDIR_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		return;

	pud = pud_offset(pgd, start);
	pgd_clear(pgd);
	pud_free_tlb(tlb, pud);
}

/*
 * This function frees user-level page tables of a process.
 *
 * Must be called with pagetable lock held.
 */
void hugetlb_free_pgd_range(struct mmu_gather **tlb,
			    unsigned long addr, unsigned long end,
			    unsigned long floor, unsigned long ceiling)
{
	pgd_t *pgd;
	unsigned long next;
	unsigned long start;

	/*
	 * Comments below take from the normal free_pgd_range().  They
	 * apply here too.  The tests against HUGEPD_MASK below are
	 * essential, because we *don't* test for this at the bottom
	 * level.  Without them we'll attempt to free a hugepte table
	 * when we unmap just part of it, even if there are other
	 * active mappings using it.
	 *
	 * The next few lines have given us lots of grief...
	 *
	 * Why are we testing HUGEPD* at this top level?  Because
	 * often there will be no work to do at all, and we'd prefer
	 * not to go all the way down to the bottom just to discover
	 * that.
	 *
	 * Why all these "- 1"s?  Because 0 represents both the bottom
	 * of the address space and the top of it (using -1 for the
	 * top wouldn't help much: the masks would do the wrong thing).
	 * The rule is that addr 0 and floor 0 refer to the bottom of
	 * the address space, but end 0 and ceiling 0 refer to the top
	 * Comparisons need to use "end - 1" and "ceiling - 1" (though
	 * that end 0 case should be mythical).
	 *
	 * Wherever addr is brought up or ceiling brought down, we
	 * must be careful to reject "the opposite 0" before it
	 * confuses the subsequent tests.  But what about where end is
	 * brought down by HUGEPD_SIZE below? no, end can't go down to
	 * 0 there.
	 *
	 * Whereas we round start (addr) and ceiling down, by different
	 * masks at different levels, in order to test whether a table
	 * now has no other vmas using it, so can be freed, we don't
	 * bother to round floor or end up - the tests don't need that.
	 */

	addr &= HUGEPD_MASK;
	if (addr < floor) {
		addr += HUGEPD_SIZE;
		if (!addr)
			return;
	}
	if (ceiling) {
		ceiling &= HUGEPD_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		end -= HUGEPD_SIZE;
	if (addr > end - 1)
		return;

	start = addr;
	pgd = pgd_offset((*tlb)->mm, addr);
	do {
		BUG_ON(! in_hugepage_area((*tlb)->mm->context, addr));
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		hugetlb_free_pud_range(*tlb, pgd, addr, next, floor, ceiling);
	} while (pgd++, addr = next, addr != end);
}

void set_huge_pte_at(struct mm_struct *mm, unsigned long addr,
		     pte_t *ptep, pte_t pte)
{
	if (pte_present(*ptep)) {
		/* We open-code pte_clear because we need to pass the right
		 * argument to hpte_update (huge / !huge)
		 */
		unsigned long old = pte_update(ptep, ~0UL);
		if (old & _PAGE_HASHPTE)
			hpte_update(mm, addr & HPAGE_MASK, ptep, old, 1);
		flush_tlb_pending();
	}
	*ptep = __pte(pte_val(pte) & ~_PAGE_HPTEFLAGS);
}

pte_t huge_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep)
{
	unsigned long old = pte_update(ptep, ~0UL);

	if (old & _PAGE_HASHPTE)
		hpte_update(mm, addr & HPAGE_MASK, ptep, old, 1);
	*ptep = __pte(0);

	return __pte(old);
}

struct slb_flush_info {
	struct mm_struct *mm;
	u16 newareas;
};

static void flush_low_segments(void *parm)
{
	struct slb_flush_info *fi = parm;
	unsigned long i;

	BUILD_BUG_ON((sizeof(fi->newareas)*8) != NUM_LOW_AREAS);

	if (current->active_mm != fi->mm)
		return;

	/* Only need to do anything if this CPU is working in the same
	 * mm as the one which has changed */

	/* update the paca copy of the context struct */
	get_paca()->context = current->active_mm->context;

	asm volatile("isync" : : : "memory");
	for (i = 0; i < NUM_LOW_AREAS; i++) {
		if (! (fi->newareas & (1U << i)))
			continue;
		asm volatile("slbie %0"
			     : : "r" ((i << SID_SHIFT) | SLBIE_C));
	}
	asm volatile("isync" : : : "memory");
}

static void flush_high_segments(void *parm)
{
	struct slb_flush_info *fi = parm;
	unsigned long i, j;


	BUILD_BUG_ON((sizeof(fi->newareas)*8) != NUM_HIGH_AREAS);

	if (current->active_mm != fi->mm)
		return;

	/* Only need to do anything if this CPU is working in the same
	 * mm as the one which has changed */

	/* update the paca copy of the context struct */
	get_paca()->context = current->active_mm->context;

	asm volatile("isync" : : : "memory");
	for (i = 0; i < NUM_HIGH_AREAS; i++) {
		if (! (fi->newareas & (1U << i)))
			continue;
		for (j = 0; j < (1UL << (HTLB_AREA_SHIFT-SID_SHIFT)); j++)
			asm volatile("slbie %0"
				     :: "r" (((i << HTLB_AREA_SHIFT)
					      + (j << SID_SHIFT)) | SLBIE_C));
	}
	asm volatile("isync" : : : "memory");
}

static int prepare_low_area_for_htlb(struct mm_struct *mm, unsigned long area)
{
	unsigned long start = area << SID_SHIFT;
	unsigned long end = (area+1) << SID_SHIFT;
	struct vm_area_struct *vma;

	BUG_ON(area >= NUM_LOW_AREAS);

	/* Check no VMAs are in the region */
	vma = find_vma(mm, start);
	if (vma && (vma->vm_start < end))
		return -EBUSY;

	return 0;
}

static int prepare_high_area_for_htlb(struct mm_struct *mm, unsigned long area)
{
	unsigned long start = area << HTLB_AREA_SHIFT;
	unsigned long end = (area+1) << HTLB_AREA_SHIFT;
	struct vm_area_struct *vma;

	BUG_ON(area >= NUM_HIGH_AREAS);

	/* Hack, so that each addresses is controlled by exactly one
	 * of the high or low area bitmaps, the first high area starts
	 * at 4GB, not 0 */
	if (start == 0)
		start = 0x100000000UL;

	/* Check no VMAs are in the region */
	vma = find_vma(mm, start);
	if (vma && (vma->vm_start < end))
		return -EBUSY;

	return 0;
}

static int open_low_hpage_areas(struct mm_struct *mm, u16 newareas)
{
	unsigned long i;
	struct slb_flush_info fi;

	BUILD_BUG_ON((sizeof(newareas)*8) != NUM_LOW_AREAS);
	BUILD_BUG_ON((sizeof(mm->context.low_htlb_areas)*8) != NUM_LOW_AREAS);

	newareas &= ~(mm->context.low_htlb_areas);
	if (! newareas)
		return 0; /* The segments we want are already open */

	for (i = 0; i < NUM_LOW_AREAS; i++)
		if ((1 << i) & newareas)
			if (prepare_low_area_for_htlb(mm, i) != 0)
				return -EBUSY;

	mm->context.low_htlb_areas |= newareas;

	/* the context change must make it to memory before the flush,
	 * so that further SLB misses do the right thing. */
	mb();

	fi.mm = mm;
	fi.newareas = newareas;
	on_each_cpu(flush_low_segments, &fi, 0, 1);

	return 0;
}

static int open_high_hpage_areas(struct mm_struct *mm, u16 newareas)
{
	struct slb_flush_info fi;
	unsigned long i;

	BUILD_BUG_ON((sizeof(newareas)*8) != NUM_HIGH_AREAS);
	BUILD_BUG_ON((sizeof(mm->context.high_htlb_areas)*8)
		     != NUM_HIGH_AREAS);

	newareas &= ~(mm->context.high_htlb_areas);
	if (! newareas)
		return 0; /* The areas we want are already open */

	for (i = 0; i < NUM_HIGH_AREAS; i++)
		if ((1 << i) & newareas)
			if (prepare_high_area_for_htlb(mm, i) != 0)
				return -EBUSY;

	mm->context.high_htlb_areas |= newareas;

	/* update the paca copy of the context struct */
	get_paca()->context = mm->context;

	/* the context change must make it to memory before the flush,
	 * so that further SLB misses do the right thing. */
	mb();

	fi.mm = mm;
	fi.newareas = newareas;
	on_each_cpu(flush_high_segments, &fi, 0, 1);

	return 0;
}

int prepare_hugepage_range(unsigned long addr, unsigned long len)
{
	int err = 0;

	if ( (addr+len) < addr )
		return -EINVAL;

	if (addr < 0x100000000UL)
		err = open_low_hpage_areas(current->mm,
					  LOW_ESID_MASK(addr, len));
	if ((addr + len) > 0x100000000UL)
		err = open_high_hpage_areas(current->mm,
					    HTLB_AREA_MASK(addr, len));
	if (err) {
		printk(KERN_DEBUG "prepare_hugepage_range(%lx, %lx)"
		       " failed (lowmask: 0x%04hx, highmask: 0x%04hx)\n",
		       addr, len,
		       LOW_ESID_MASK(addr, len), HTLB_AREA_MASK(addr, len));
		return err;
	}

	return 0;
}

struct page *
follow_huge_addr(struct mm_struct *mm, unsigned long address, int write)
{
	pte_t *ptep;
	struct page *page;

	if (! in_hugepage_area(mm->context, address))
		return ERR_PTR(-EINVAL);

	ptep = huge_pte_offset(mm, address);
	page = pte_page(*ptep);
	if (page)
		page += (address % HPAGE_SIZE) / PAGE_SIZE;

	return page;
}

int pmd_huge(pmd_t pmd)
{
	return 0;
}

struct page *
follow_huge_pmd(struct mm_struct *mm, unsigned long address,
		pmd_t *pmd, int write)
{
	BUG();
	return NULL;
}

/* Because we have an exclusive hugepage region which lies within the
 * normal user address space, we have to take special measures to make
 * non-huge mmap()s evade the hugepage reserved regions. */
unsigned long arch_get_unmapped_area(struct file *filp, unsigned long addr,
				     unsigned long len, unsigned long pgoff,
				     unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long start_addr;

	if (len > TASK_SIZE)
		return -ENOMEM;

	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = find_vma(mm, addr);
		if (((TASK_SIZE - len) >= addr)
		    && (!vma || (addr+len) <= vma->vm_start)
		    && !is_hugepage_only_range(mm, addr,len))
			return addr;
	}
	if (len > mm->cached_hole_size) {
	        start_addr = addr = mm->free_area_cache;
	} else {
	        start_addr = addr = TASK_UNMAPPED_BASE;
	        mm->cached_hole_size = 0;
	}

full_search:
	vma = find_vma(mm, addr);
	while (TASK_SIZE - len >= addr) {
		BUG_ON(vma && (addr >= vma->vm_end));

		if (touches_hugepage_low_range(mm, addr, len)) {
			addr = ALIGN(addr+1, 1<<SID_SHIFT);
			vma = find_vma(mm, addr);
			continue;
		}
		if (touches_hugepage_high_range(mm, addr, len)) {
			addr = ALIGN(addr+1, 1UL<<HTLB_AREA_SHIFT);
			vma = find_vma(mm, addr);
			continue;
		}
		if (!vma || addr + len <= vma->vm_start) {
			/*
			 * Remember the place where we stopped the search:
			 */
			mm->free_area_cache = addr + len;
			return addr;
		}
		if (addr + mm->cached_hole_size < vma->vm_start)
		        mm->cached_hole_size = vma->vm_start - addr;
		addr = vma->vm_end;
		vma = vma->vm_next;
	}

	/* Make sure we didn't miss any holes */
	if (start_addr != TASK_UNMAPPED_BASE) {
		start_addr = addr = TASK_UNMAPPED_BASE;
		mm->cached_hole_size = 0;
		goto full_search;
	}
	return -ENOMEM;
}

/*
 * This mmap-allocator allocates new areas top-down from below the
 * stack's low limit (the base):
 *
 * Because we have an exclusive hugepage region which lies within the
 * normal user address space, we have to take special measures to make
 * non-huge mmap()s evade the hugepage reserved regions.
 */
unsigned long
arch_get_unmapped_area_topdown(struct file *filp, const unsigned long addr0,
			  const unsigned long len, const unsigned long pgoff,
			  const unsigned long flags)
{
	struct vm_area_struct *vma, *prev_vma;
	struct mm_struct *mm = current->mm;
	unsigned long base = mm->mmap_base, addr = addr0;
	unsigned long largest_hole = mm->cached_hole_size;
	int first_time = 1;

	/* requested length too big for entire address space */
	if (len > TASK_SIZE)
		return -ENOMEM;

	/* dont allow allocations above current base */
	if (mm->free_area_cache > base)
		mm->free_area_cache = base;

	/* requesting a specific address */
	if (addr) {
		addr = PAGE_ALIGN(addr);
		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr &&
				(!vma || addr + len <= vma->vm_start)
				&& !is_hugepage_only_range(mm, addr,len))
			return addr;
	}

	if (len <= largest_hole) {
	        largest_hole = 0;
		mm->free_area_cache = base;
	}
try_again:
	/* make sure it can fit in the remaining address space */
	if (mm->free_area_cache < len)
		goto fail;

	/* either no address requested or cant fit in requested address hole */
	addr = (mm->free_area_cache - len) & PAGE_MASK;
	do {
hugepage_recheck:
		if (touches_hugepage_low_range(mm, addr, len)) {
			addr = (addr & ((~0) << SID_SHIFT)) - len;
			goto hugepage_recheck;
		} else if (touches_hugepage_high_range(mm, addr, len)) {
			addr = (addr & ((~0UL) << HTLB_AREA_SHIFT)) - len;
			goto hugepage_recheck;
		}

		/*
		 * Lookup failure means no vma is above this address,
		 * i.e. return with success:
		 */
 	 	if (!(vma = find_vma_prev(mm, addr, &prev_vma)))
			return addr;

		/*
		 * new region fits between prev_vma->vm_end and
		 * vma->vm_start, use it:
		 */
		if (addr+len <= vma->vm_start &&
		          (!prev_vma || (addr >= prev_vma->vm_end))) {
			/* remember the address as a hint for next time */
		        mm->cached_hole_size = largest_hole;
		        return (mm->free_area_cache = addr);
		} else {
			/* pull free_area_cache down to the first hole */
		        if (mm->free_area_cache == vma->vm_end) {
				mm->free_area_cache = vma->vm_start;
				mm->cached_hole_size = largest_hole;
			}
		}

		/* remember the largest hole we saw so far */
		if (addr + largest_hole < vma->vm_start)
		        largest_hole = vma->vm_start - addr;

		/* try just below the current vma->vm_start */
		addr = vma->vm_start-len;
	} while (len <= vma->vm_start);

fail:
	/*
	 * if hint left us with no space for the requested
	 * mapping then try again:
	 */
	if (first_time) {
		mm->free_area_cache = base;
		largest_hole = 0;
		first_time = 0;
		goto try_again;
	}
	/*
	 * A failed mmap() very likely causes application failure,
	 * so fall back to the bottom-up function here. This scenario
	 * can happen with large stack limits and large mmap()
	 * allocations.
	 */
	mm->free_area_cache = TASK_UNMAPPED_BASE;
	mm->cached_hole_size = ~0UL;
	addr = arch_get_unmapped_area(filp, addr0, len, pgoff, flags);
	/*
	 * Restore the topdown base:
	 */
	mm->free_area_cache = base;
	mm->cached_hole_size = ~0UL;

	return addr;
}

static int htlb_check_hinted_area(unsigned long addr, unsigned long len)
{
	struct vm_area_struct *vma;

	vma = find_vma(current->mm, addr);
	if (!vma || ((addr + len) <= vma->vm_start))
		return 0;

	return -ENOMEM;
}

static unsigned long htlb_get_low_area(unsigned long len, u16 segmask)
{
	unsigned long addr = 0;
	struct vm_area_struct *vma;

	vma = find_vma(current->mm, addr);
	while (addr + len <= 0x100000000UL) {
		BUG_ON(vma && (addr >= vma->vm_end)); /* invariant */

		if (! __within_hugepage_low_range(addr, len, segmask)) {
			addr = ALIGN(addr+1, 1<<SID_SHIFT);
			vma = find_vma(current->mm, addr);
			continue;
		}

		if (!vma || (addr + len) <= vma->vm_start)
			return addr;
		addr = ALIGN(vma->vm_end, HPAGE_SIZE);
		/* Depending on segmask this might not be a confirmed
		 * hugepage region, so the ALIGN could have skipped
		 * some VMAs */
		vma = find_vma(current->mm, addr);
	}

	return -ENOMEM;
}

static unsigned long htlb_get_high_area(unsigned long len, u16 areamask)
{
	unsigned long addr = 0x100000000UL;
	struct vm_area_struct *vma;

	vma = find_vma(current->mm, addr);
	while (addr + len <= TASK_SIZE_USER64) {
		BUG_ON(vma && (addr >= vma->vm_end)); /* invariant */

		if (! __within_hugepage_high_range(addr, len, areamask)) {
			addr = ALIGN(addr+1, 1UL<<HTLB_AREA_SHIFT);
			vma = find_vma(current->mm, addr);
			continue;
		}

		if (!vma || (addr + len) <= vma->vm_start)
			return addr;
		addr = ALIGN(vma->vm_end, HPAGE_SIZE);
		/* Depending on segmask this might not be a confirmed
		 * hugepage region, so the ALIGN could have skipped
		 * some VMAs */
		vma = find_vma(current->mm, addr);
	}

	return -ENOMEM;
}

unsigned long hugetlb_get_unmapped_area(struct file *file, unsigned long addr,
					unsigned long len, unsigned long pgoff,
					unsigned long flags)
{
	int lastshift;
	u16 areamask, curareas;

	if (HPAGE_SHIFT == 0)
		return -EINVAL;
	if (len & ~HPAGE_MASK)
		return -EINVAL;

	if (!cpu_has_feature(CPU_FTR_16M_PAGE))
		return -EINVAL;

	/* Paranoia, caller should have dealt with this */
	BUG_ON((addr + len)  < addr);

	if (test_thread_flag(TIF_32BIT)) {
		/* Paranoia, caller should have dealt with this */
		BUG_ON((addr + len) > 0x100000000UL);

		curareas = current->mm->context.low_htlb_areas;

		/* First see if we can use the hint address */
		if (addr && (htlb_check_hinted_area(addr, len) == 0)) {
			areamask = LOW_ESID_MASK(addr, len);
			if (open_low_hpage_areas(current->mm, areamask) == 0)
				return addr;
		}

		/* Next see if we can map in the existing low areas */
		addr = htlb_get_low_area(len, curareas);
		if (addr != -ENOMEM)
			return addr;

		/* Finally go looking for areas to open */
		lastshift = 0;
		for (areamask = LOW_ESID_MASK(0x100000000UL-len, len);
		     ! lastshift; areamask >>=1) {
			if (areamask & 1)
				lastshift = 1;

			addr = htlb_get_low_area(len, curareas | areamask);
			if ((addr != -ENOMEM)
			    && open_low_hpage_areas(current->mm, areamask) == 0)
				return addr;
		}
	} else {
		curareas = current->mm->context.high_htlb_areas;

		/* First see if we can use the hint address */
		/* We discourage 64-bit processes from doing hugepage
		 * mappings below 4GB (must use MAP_FIXED) */
		if ((addr >= 0x100000000UL)
		    && (htlb_check_hinted_area(addr, len) == 0)) {
			areamask = HTLB_AREA_MASK(addr, len);
			if (open_high_hpage_areas(current->mm, areamask) == 0)
				return addr;
		}

		/* Next see if we can map in the existing high areas */
		addr = htlb_get_high_area(len, curareas);
		if (addr != -ENOMEM)
			return addr;

		/* Finally go looking for areas to open */
		lastshift = 0;
		for (areamask = HTLB_AREA_MASK(TASK_SIZE_USER64-len, len);
		     ! lastshift; areamask >>=1) {
			if (areamask & 1)
				lastshift = 1;

			addr = htlb_get_high_area(len, curareas | areamask);
			if ((addr != -ENOMEM)
			    && open_high_hpage_areas(current->mm, areamask) == 0)
				return addr;
		}
	}
	printk(KERN_DEBUG "hugetlb_get_unmapped_area() unable to open"
	       " enough areas\n");
	return -ENOMEM;
}

/*
 * Called by asm hashtable.S for doing lazy icache flush
 */
static unsigned int hash_huge_page_do_lazy_icache(unsigned long rflags,
						  pte_t pte, int trap)
{
	struct page *page;
	int i;

	if (!pfn_valid(pte_pfn(pte)))
		return rflags;

	page = pte_page(pte);

	/* page is dirty */
	if (!test_bit(PG_arch_1, &page->flags) && !PageReserved(page)) {
		if (trap == 0x400) {
			for (i = 0; i < (HPAGE_SIZE / PAGE_SIZE); i++)
				__flush_dcache_icache(page_address(page+i));
			set_bit(PG_arch_1, &page->flags);
		} else {
			rflags |= HPTE_R_N;
		}
	}
	return rflags;
}

int hash_huge_page(struct mm_struct *mm, unsigned long access,
		   unsigned long ea, unsigned long vsid, int local,
		   unsigned long trap)
{
	pte_t *ptep;
	unsigned long old_pte, new_pte;
	unsigned long va, rflags, pa;
	long slot;
	int err = 1;

	ptep = huge_pte_offset(mm, ea);

	/* Search the Linux page table for a match with va */
	va = (vsid << 28) | (ea & 0x0fffffff);

	/*
	 * If no pte found or not present, send the problem up to
	 * do_page_fault
	 */
	if (unlikely(!ptep || pte_none(*ptep)))
		goto out;

	/* 
	 * Check the user's access rights to the page.  If access should be
	 * prevented then send the problem up to do_page_fault.
	 */
	if (unlikely(access & ~pte_val(*ptep)))
		goto out;
	/*
	 * At this point, we have a pte (old_pte) which can be used to build
	 * or update an HPTE. There are 2 cases:
	 *
	 * 1. There is a valid (present) pte with no associated HPTE (this is 
	 *	the most common case)
	 * 2. There is a valid (present) pte with an associated HPTE. The
	 *	current values of the pp bits in the HPTE prevent access
	 *	because we are doing software DIRTY bit management and the
	 *	page is currently not DIRTY. 
	 */


	do {
		old_pte = pte_val(*ptep);
		if (old_pte & _PAGE_BUSY)
			goto out;
		new_pte = old_pte | _PAGE_BUSY |
			_PAGE_ACCESSED | _PAGE_HASHPTE;
	} while(old_pte != __cmpxchg_u64((unsigned long *)ptep,
					 old_pte, new_pte));

	rflags = 0x2 | (!(new_pte & _PAGE_RW));
 	/* _PAGE_EXEC -> HW_NO_EXEC since it's inverted */
	rflags |= ((new_pte & _PAGE_EXEC) ? 0 : HPTE_R_N);
	if (!cpu_has_feature(CPU_FTR_COHERENT_ICACHE))
		/* No CPU has hugepages but lacks no execute, so we
		 * don't need to worry about that case */
		rflags = hash_huge_page_do_lazy_icache(rflags, __pte(old_pte),
						       trap);

	/* Check if pte already has an hpte (case 2) */
	if (unlikely(old_pte & _PAGE_HASHPTE)) {
		/* There MIGHT be an HPTE for this pte */
		unsigned long hash, slot;

		hash = hpt_hash(va, HPAGE_SHIFT);
		if (old_pte & _PAGE_F_SECOND)
			hash = ~hash;
		slot = (hash & htab_hash_mask) * HPTES_PER_GROUP;
		slot += (old_pte & _PAGE_F_GIX) >> 12;

		if (ppc_md.hpte_updatepp(slot, rflags, va, mmu_huge_psize,
					 local) == -1)
			old_pte &= ~_PAGE_HPTEFLAGS;
	}

	if (likely(!(old_pte & _PAGE_HASHPTE))) {
		unsigned long hash = hpt_hash(va, HPAGE_SHIFT);
		unsigned long hpte_group;

		pa = pte_pfn(__pte(old_pte)) << PAGE_SHIFT;

repeat:
		hpte_group = ((hash & htab_hash_mask) *
			      HPTES_PER_GROUP) & ~0x7UL;

		/* clear HPTE slot informations in new PTE */
		new_pte = (new_pte & ~_PAGE_HPTEFLAGS) | _PAGE_HASHPTE;

		/* Add in WIMG bits */
		/* XXX We should store these in the pte */
		/* --BenH: I think they are ... */
		rflags |= _PAGE_COHERENT;

		/* Insert into the hash table, primary slot */
		slot = ppc_md.hpte_insert(hpte_group, va, pa, rflags, 0,
					  mmu_huge_psize);

		/* Primary is full, try the secondary */
		if (unlikely(slot == -1)) {
			new_pte |= _PAGE_F_SECOND;
			hpte_group = ((~hash & htab_hash_mask) *
				      HPTES_PER_GROUP) & ~0x7UL; 
			slot = ppc_md.hpte_insert(hpte_group, va, pa, rflags,
						  HPTE_V_SECONDARY,
						  mmu_huge_psize);
			if (slot == -1) {
				if (mftb() & 0x1)
					hpte_group = ((hash & htab_hash_mask) *
						      HPTES_PER_GROUP)&~0x7UL;

				ppc_md.hpte_remove(hpte_group);
				goto repeat;
                        }
		}

		if (unlikely(slot == -2))
			panic("hash_huge_page: pte_insert failed\n");

		new_pte |= (slot << 12) & _PAGE_F_GIX;
	}

	/*
	 * No need to use ldarx/stdcx here
	 */
	*ptep = __pte(new_pte & ~_PAGE_BUSY);

	err = 0;

 out:
	return err;
}

static void zero_ctor(void *addr, kmem_cache_t *cache, unsigned long flags)
{
	memset(addr, 0, kmem_cache_size(cache));
}

static int __init hugetlbpage_init(void)
{
	if (!cpu_has_feature(CPU_FTR_16M_PAGE))
		return -ENODEV;

	huge_pgtable_cache = kmem_cache_create("hugepte_cache",
					       HUGEPTE_TABLE_SIZE,
					       HUGEPTE_TABLE_SIZE,
					       SLAB_HWCACHE_ALIGN |
					       SLAB_MUST_HWCACHE_ALIGN,
					       zero_ctor, NULL);
	if (! huge_pgtable_cache)
		panic("hugetlbpage_init(): could not create hugepte cache\n");

	return 0;
}

module_init(hugetlbpage_init);
