/*
 *  linux/arch/i386/mm/pgtable.c
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/swap.h>
#include <linux/smp.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/quicklist.h>

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/fixmap.h>
#include <asm/e820.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

void show_mem(void)
{
	int total = 0, reserved = 0;
	int shared = 0, cached = 0;
	int highmem = 0;
	struct page *page;
	pg_data_t *pgdat;
	unsigned long i;
	unsigned long flags;

	printk(KERN_INFO "Mem-info:\n");
	show_free_areas();
	printk(KERN_INFO "Free swap:       %6ldkB\n", nr_swap_pages<<(PAGE_SHIFT-10));
	for_each_online_pgdat(pgdat) {
		pgdat_resize_lock(pgdat, &flags);
		for (i = 0; i < pgdat->node_spanned_pages; ++i) {
			if (unlikely(i % MAX_ORDER_NR_PAGES == 0))
				touch_nmi_watchdog();
			page = pgdat_page_nr(pgdat, i);
			total++;
			if (PageHighMem(page))
				highmem++;
			if (PageReserved(page))
				reserved++;
			else if (PageSwapCache(page))
				cached++;
			else if (page_count(page))
				shared += page_count(page) - 1;
		}
		pgdat_resize_unlock(pgdat, &flags);
	}
	printk(KERN_INFO "%d pages of RAM\n", total);
	printk(KERN_INFO "%d pages of HIGHMEM\n", highmem);
	printk(KERN_INFO "%d reserved pages\n", reserved);
	printk(KERN_INFO "%d pages shared\n", shared);
	printk(KERN_INFO "%d pages swap cached\n", cached);

	printk(KERN_INFO "%lu pages dirty\n", global_page_state(NR_FILE_DIRTY));
	printk(KERN_INFO "%lu pages writeback\n",
					global_page_state(NR_WRITEBACK));
	printk(KERN_INFO "%lu pages mapped\n", global_page_state(NR_FILE_MAPPED));
	printk(KERN_INFO "%lu pages slab\n",
		global_page_state(NR_SLAB_RECLAIMABLE) +
		global_page_state(NR_SLAB_UNRECLAIMABLE));
	printk(KERN_INFO "%lu pages pagetables\n",
					global_page_state(NR_PAGETABLE));
}

/*
 * Associate a virtual page frame with a given physical page frame 
 * and protection flags for that frame.
 */ 
static void set_pte_pfn(unsigned long vaddr, unsigned long pfn, pgprot_t flags)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = swapper_pg_dir + pgd_index(vaddr);
	if (pgd_none(*pgd)) {
		BUG();
		return;
	}
	pud = pud_offset(pgd, vaddr);
	if (pud_none(*pud)) {
		BUG();
		return;
	}
	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd)) {
		BUG();
		return;
	}
	pte = pte_offset_kernel(pmd, vaddr);
	if (pgprot_val(flags))
		set_pte_present(&init_mm, vaddr, pte, pfn_pte(pfn, flags));
	else
		pte_clear(&init_mm, vaddr, pte);

	/*
	 * It's enough to flush this one mapping.
	 * (PGE mappings get flushed as well)
	 */
	__flush_tlb_one(vaddr);
}

/*
 * Associate a large virtual page frame with a given physical page frame 
 * and protection flags for that frame. pfn is for the base of the page,
 * vaddr is what the page gets mapped to - both must be properly aligned. 
 * The pmd must already be instantiated. Assumes PAE mode.
 */ 
void set_pmd_pfn(unsigned long vaddr, unsigned long pfn, pgprot_t flags)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	if (vaddr & (PMD_SIZE-1)) {		/* vaddr is misaligned */
		printk(KERN_WARNING "set_pmd_pfn: vaddr misaligned\n");
		return; /* BUG(); */
	}
	if (pfn & (PTRS_PER_PTE-1)) {		/* pfn is misaligned */
		printk(KERN_WARNING "set_pmd_pfn: pfn misaligned\n");
		return; /* BUG(); */
	}
	pgd = swapper_pg_dir + pgd_index(vaddr);
	if (pgd_none(*pgd)) {
		printk(KERN_WARNING "set_pmd_pfn: pgd_none\n");
		return; /* BUG(); */
	}
	pud = pud_offset(pgd, vaddr);
	pmd = pmd_offset(pud, vaddr);
	set_pmd(pmd, pfn_pmd(pfn, flags));
	/*
	 * It's enough to flush this one mapping.
	 * (PGE mappings get flushed as well)
	 */
	__flush_tlb_one(vaddr);
}

static int fixmaps;
unsigned long __FIXADDR_TOP = 0xfffff000;
EXPORT_SYMBOL(__FIXADDR_TOP);

void __set_fixmap (enum fixed_addresses idx, unsigned long phys, pgprot_t flags)
{
	unsigned long address = __fix_to_virt(idx);

	if (idx >= __end_of_fixed_addresses) {
		BUG();
		return;
	}
	set_pte_pfn(address, phys >> PAGE_SHIFT, flags);
	fixmaps++;
}

/**
 * reserve_top_address - reserves a hole in the top of kernel address space
 * @reserve - size of hole to reserve
 *
 * Can be used to relocate the fixmap area and poke a hole in the top
 * of kernel address space to make room for a hypervisor.
 */
void reserve_top_address(unsigned long reserve)
{
	BUG_ON(fixmaps > 0);
	printk(KERN_INFO "Reserving virtual address space above 0x%08x\n",
	       (int)-reserve);
	__FIXADDR_TOP = -reserve - PAGE_SIZE;
	__VMALLOC_RESERVE += reserve;
}

pte_t *pte_alloc_one_kernel(struct mm_struct *mm, unsigned long address)
{
	return (pte_t *)__get_free_page(GFP_KERNEL|__GFP_REPEAT|__GFP_ZERO);
}

pgtable_t pte_alloc_one(struct mm_struct *mm, unsigned long address)
{
	struct page *pte;

#ifdef CONFIG_HIGHPTE
	pte = alloc_pages(GFP_KERNEL|__GFP_HIGHMEM|__GFP_REPEAT|__GFP_ZERO, 0);
#else
	pte = alloc_pages(GFP_KERNEL|__GFP_REPEAT|__GFP_ZERO, 0);
#endif
	if (pte)
		pgtable_page_ctor(pte);
	return pte;
}

/*
 * List of all pgd's needed for non-PAE so it can invalidate entries
 * in both cached and uncached pgd's; not needed for PAE since the
 * kernel pmd is shared. If PAE were not to share the pmd a similar
 * tactic would be needed. This is essentially codepath-based locking
 * against pageattr.c; it is the unique case in which a valid change
 * of kernel pagetables can't be lazily synchronized by vmalloc faults.
 * vmalloc faults work because attached pagetables are never freed.
 * -- wli
 */
static inline void pgd_list_add(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	list_add(&page->lru, &pgd_list);
}

static inline void pgd_list_del(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	list_del(&page->lru);
}

#define UNSHARED_PTRS_PER_PGD				\
	(SHARED_KERNEL_PMD ? USER_PTRS_PER_PGD : PTRS_PER_PGD)

static void pgd_ctor(void *p)
{
	pgd_t *pgd = p;
	unsigned long flags;

	/* Clear usermode parts of PGD */
	memset(pgd, 0, USER_PTRS_PER_PGD*sizeof(pgd_t));

	spin_lock_irqsave(&pgd_lock, flags);

	/* If the pgd points to a shared pagetable level (either the
	   ptes in non-PAE, or shared PMD in PAE), then just copy the
	   references from swapper_pg_dir. */
	if (PAGETABLE_LEVELS == 2 ||
	    (PAGETABLE_LEVELS == 3 && SHARED_KERNEL_PMD)) {
		clone_pgd_range(pgd + USER_PTRS_PER_PGD,
				swapper_pg_dir + USER_PTRS_PER_PGD,
				KERNEL_PGD_PTRS);
		paravirt_alloc_pd_clone(__pa(pgd) >> PAGE_SHIFT,
					__pa(swapper_pg_dir) >> PAGE_SHIFT,
					USER_PTRS_PER_PGD,
					KERNEL_PGD_PTRS);
	}

	/* list required to sync kernel mapping updates */
	if (!SHARED_KERNEL_PMD)
		pgd_list_add(pgd);

	spin_unlock_irqrestore(&pgd_lock, flags);
}

static void pgd_dtor(void *pgd)
{
	unsigned long flags; /* can be called from interrupt context */

	if (SHARED_KERNEL_PMD)
		return;

	spin_lock_irqsave(&pgd_lock, flags);
	pgd_list_del(pgd);
	spin_unlock_irqrestore(&pgd_lock, flags);
}

#ifdef CONFIG_X86_PAE
/*
 * Mop up any pmd pages which may still be attached to the pgd.
 * Normally they will be freed by munmap/exit_mmap, but any pmd we
 * preallocate which never got a corresponding vma will need to be
 * freed manually.
 */
static void pgd_mop_up_pmds(struct mm_struct *mm, pgd_t *pgdp)
{
	int i;

	for(i = 0; i < UNSHARED_PTRS_PER_PGD; i++) {
		pgd_t pgd = pgdp[i];

		if (pgd_val(pgd) != 0) {
			pmd_t *pmd = (pmd_t *)pgd_page_vaddr(pgd);

			pgdp[i] = native_make_pgd(0);

			paravirt_release_pd(pgd_val(pgd) >> PAGE_SHIFT);
			pmd_free(mm, pmd);
		}
	}
}

/*
 * In PAE mode, we need to do a cr3 reload (=tlb flush) when
 * updating the top-level pagetable entries to guarantee the
 * processor notices the update.  Since this is expensive, and
 * all 4 top-level entries are used almost immediately in a
 * new process's life, we just pre-populate them here.
 *
 * Also, if we're in a paravirt environment where the kernel pmd is
 * not shared between pagetables (!SHARED_KERNEL_PMDS), we allocate
 * and initialize the kernel pmds here.
 */
static int pgd_prepopulate_pmd(struct mm_struct *mm, pgd_t *pgd)
{
	pud_t *pud;
	unsigned long addr;
	int i;

	pud = pud_offset(pgd, 0);
 	for (addr = i = 0; i < UNSHARED_PTRS_PER_PGD;
	     i++, pud++, addr += PUD_SIZE) {
		pmd_t *pmd = pmd_alloc_one(mm, addr);

		if (!pmd) {
			pgd_mop_up_pmds(mm, pgd);
			return 0;
		}

		if (i >= USER_PTRS_PER_PGD)
			memcpy(pmd, (pmd_t *)pgd_page_vaddr(swapper_pg_dir[i]),
			       sizeof(pmd_t) * PTRS_PER_PMD);

		pud_populate(mm, pud, pmd);
	}

	return 1;
}
#else  /* !CONFIG_X86_PAE */
/* No need to prepopulate any pagetable entries in non-PAE modes. */
static int pgd_prepopulate_pmd(struct mm_struct *mm, pgd_t *pgd)
{
	return 1;
}

static void pgd_mop_up_pmds(struct mm_struct *mm, pgd_t *pgdp)
{
}
#endif	/* CONFIG_X86_PAE */

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd = (pgd_t *)__get_free_page(GFP_KERNEL | __GFP_ZERO);

	/* so that alloc_pd can use it */
	mm->pgd = pgd;
	if (pgd)
		pgd_ctor(pgd);

	if (pgd && !pgd_prepopulate_pmd(mm, pgd)) {
		pgd_dtor(pgd);
		free_page((unsigned long)pgd);
		pgd = NULL;
	}

	return pgd;
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	pgd_mop_up_pmds(mm, pgd);
	pgd_dtor(pgd);
	free_page((unsigned long)pgd);
}

void __pte_free_tlb(struct mmu_gather *tlb, struct page *pte)
{
	pgtable_page_dtor(pte);
	paravirt_release_pt(page_to_pfn(pte));
	tlb_remove_page(tlb, pte);
}

#ifdef CONFIG_X86_PAE

void __pmd_free_tlb(struct mmu_gather *tlb, pmd_t *pmd)
{
	paravirt_release_pd(__pa(pmd) >> PAGE_SHIFT);
	tlb_remove_page(tlb, virt_to_page(pmd));
}

#endif
