/*
 *	Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 *
 * This file contains the lowest level x86_64-specific interrupt
 * entry and irq statistics code. All the remaining irq logic is
 * done by the generic kernel/irq/ code and in the
 * x86_64-specific irq controller code. (e.g. i8259.c and
 * io_apic.c.)
 */

#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <asm/io_apic.h>
#include <asm/idle.h>
#include <asm/smp.h>

#ifdef CONFIG_DEBUG_STACKOVERFLOW
/*
 * Probabilistic stack overflow check:
 *
 * Only check the stack in process context, because everything else
 * runs on the big interrupt stacks. Checking reliably is too expensive,
 * so we just check from interrupts.
 */
static inline void stack_overflow_check(struct pt_regs *regs)
{
	u64 curbase = (u64)task_stack_page(current);
	static unsigned long warned = -60*HZ;

	if (regs->sp >= curbase && regs->sp <= curbase + THREAD_SIZE &&
	    regs->sp <  curbase + sizeof(struct thread_info) + 128 &&
	    time_after(jiffies, warned + 60*HZ)) {
		printk("do_IRQ: %s near stack overflow (cur:%Lx,sp:%lx)\n",
		       current->comm, curbase, regs->sp);
		show_stack(NULL,NULL);
		warned = jiffies;
	}
}
#endif

/*
 * do_IRQ handles all normal device IRQ's (the special
 * SMP cross-CPU interrupts have their own specific
 * handlers).
 */
asmlinkage unsigned int do_IRQ(struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	struct irq_desc *desc;

	/* high bit used in ret_from_ code  */
	unsigned vector = ~regs->orig_ax;
	unsigned irq;

	exit_idle();
	irq_enter();
	irq = __get_cpu_var(vector_irq)[vector];

#ifdef CONFIG_DEBUG_STACKOVERFLOW
	stack_overflow_check(regs);
#endif

	desc = irq_to_desc(irq);
	if (likely(desc))
		generic_handle_irq_desc(irq, desc);
	else {
		if (!disable_apic)
			ack_APIC_irq();

		if (printk_ratelimit())
			printk(KERN_EMERG "%s: %d.%d No irq handler for vector\n",
				__func__, smp_processor_id(), vector);
	}

	irq_exit();

	set_irq_regs(old_regs);
	return 1;
}

#ifdef CONFIG_HOTPLUG_CPU
void fixup_irqs(cpumask_t map)
{
	unsigned int irq;
	static int warned;
	struct irq_desc *desc;

	for_each_irq_desc(irq, desc) {
		cpumask_t mask;
		int break_affinity = 0;
		int set_affinity = 1;

		if (irq == 2)
			continue;

		/* interrupt's are disabled at this point */
		spin_lock(&desc->lock);

		if (!irq_has_action(irq) ||
		    cpus_equal(desc->affinity, map)) {
			spin_unlock(&desc->lock);
			continue;
		}

		cpus_and(mask, desc->affinity, map);
		if (cpus_empty(mask)) {
			break_affinity = 1;
			mask = map;
		}

		if (desc->chip->mask)
			desc->chip->mask(irq);

		if (desc->chip->set_affinity)
			desc->chip->set_affinity(irq, mask);
		else if (!(warned++))
			set_affinity = 0;

		if (desc->chip->unmask)
			desc->chip->unmask(irq);

		spin_unlock(&desc->lock);

		if (break_affinity && set_affinity)
			printk("Broke affinity for irq %i\n", irq);
		else if (!set_affinity)
			printk("Cannot set affinity for irq %i\n", irq);
	}

	/* That doesn't seem sufficient.  Give it 1ms. */
	local_irq_enable();
	mdelay(1);
	local_irq_disable();
}
#endif

extern void call_softirq(void);

asmlinkage void do_softirq(void)
{
 	__u32 pending;
 	unsigned long flags;

 	if (in_interrupt())
 		return;

 	local_irq_save(flags);
 	pending = local_softirq_pending();
 	/* Switch to interrupt stack */
 	if (pending) {
		call_softirq();
		WARN_ON_ONCE(softirq_count());
	}
 	local_irq_restore(flags);
}
