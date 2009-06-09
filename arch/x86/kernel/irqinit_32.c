#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/sysdev.h>
#include <linux/bitops.h>

#include <asm/atomic.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/timer.h>
#include <asm/pgtable.h>
#include <asm/delay.h>
#include <asm/desc.h>
#include <asm/apic.h>
#include <asm/arch_hooks.h>
#include <asm/i8259.h>



/*
 * Note that on a 486, we don't want to do a SIGFPE on an irq13
 * as the irq is unreliable, and exception 16 works correctly
 * (ie as explained in the intel literature). On a 386, you
 * can't use exception 16 due to bad IBM design, so we have to
 * rely on the less exact irq13.
 *
 * Careful.. Not only is IRQ13 unreliable, but it is also
 * leads to races. IBM designers who came up with it should
 * be shot.
 */
 

static irqreturn_t math_error_irq(int cpl, void *dev_id)
{
	extern void math_error(void __user *);
	outb(0,0xF0);
	if (ignore_fpu_irq || !boot_cpu_data.hard_math)
		return IRQ_NONE;
	math_error((void __user *)get_irq_regs()->ip);
	return IRQ_HANDLED;
}

/*
 * New motherboards sometimes make IRQ 13 be a PCI interrupt,
 * so allow interrupt sharing.
 */
static struct irqaction fpu_irq = {
	.handler = math_error_irq,
	.mask = CPU_MASK_NONE,
	.name = "fpu",
};

void __init init_ISA_irqs (void)
{
	int i;

#ifdef CONFIG_X86_LOCAL_APIC
	init_bsp_APIC();
#endif
	init_8259A(0);

	/*
	 * 16 old-style INTA-cycle interrupts:
	 */
	for (i = 0; i < 16; i++) {
		/* first time call this irq_desc */
		struct irq_desc *desc = irq_to_desc(i);

		desc->status = IRQ_DISABLED;
		desc->action = NULL;
		desc->depth = 1;

		set_irq_chip_and_handler_name(i, &i8259A_chip,
					      handle_level_irq, "XT");
	}
}

/*
 * IRQ2 is cascade interrupt to second interrupt controller
 */
static struct irqaction irq2 = {
	.handler = no_action,
	.mask = CPU_MASK_NONE,
	.name = "cascade",
};

DEFINE_PER_CPU(vector_irq_t, vector_irq) = {
	[0 ... IRQ0_VECTOR - 1] = -1,
	[IRQ0_VECTOR] = 0,
	[IRQ1_VECTOR] = 1,
	[IRQ2_VECTOR] = 2,
	[IRQ3_VECTOR] = 3,
	[IRQ4_VECTOR] = 4,
	[IRQ5_VECTOR] = 5,
	[IRQ6_VECTOR] = 6,
	[IRQ7_VECTOR] = 7,
	[IRQ8_VECTOR] = 8,
	[IRQ9_VECTOR] = 9,
	[IRQ10_VECTOR] = 10,
	[IRQ11_VECTOR] = 11,
	[IRQ12_VECTOR] = 12,
	[IRQ13_VECTOR] = 13,
	[IRQ14_VECTOR] = 14,
	[IRQ15_VECTOR] = 15,
	[IRQ15_VECTOR + 1 ... NR_VECTORS - 1] = -1
};

/* Overridden in paravirt.c */
void init_IRQ(void) __attribute__((weak, alias("native_init_IRQ")));

void __init native_init_IRQ(void)
{
	int i;

	/* all the set up before the call gates are initialised */
	pre_intr_init_hook();

	/*
	 * Cover the whole vector space, no vector can escape
	 * us. (some of these will be overridden and become
	 * 'special' SMP interrupts)
	 */
	for (i =  FIRST_EXTERNAL_VECTOR; i < NR_VECTORS; i++) {
		/* SYSCALL_VECTOR was reserved in trap_init. */
		if (i != SYSCALL_VECTOR)
			set_intr_gate(i, interrupt[i]);
	}


#if defined(CONFIG_X86_LOCAL_APIC) && defined(CONFIG_SMP)
	/*
	 * The reschedule interrupt is a CPU-to-CPU reschedule-helper
	 * IPI, driven by wakeup.
	 */
	alloc_intr_gate(RESCHEDULE_VECTOR, reschedule_interrupt);

	/* IPI for invalidation */
	alloc_intr_gate(INVALIDATE_TLB_VECTOR, invalidate_interrupt);

	/* IPI for generic function call */
	alloc_intr_gate(CALL_FUNCTION_VECTOR, call_function_interrupt);

	/* IPI for single call function */
	set_intr_gate(CALL_FUNCTION_SINGLE_VECTOR, call_function_single_interrupt);

	/* Low priority IPI to cleanup after moving an irq */
	set_intr_gate(IRQ_MOVE_CLEANUP_VECTOR, irq_move_cleanup_interrupt);
#endif

#ifdef CONFIG_X86_LOCAL_APIC
	/* self generated IPI for local APIC timer */
	alloc_intr_gate(LOCAL_TIMER_VECTOR, apic_timer_interrupt);

	/* IPI vectors for APIC spurious and error interrupts */
	alloc_intr_gate(SPURIOUS_APIC_VECTOR, spurious_interrupt);
	alloc_intr_gate(ERROR_APIC_VECTOR, error_interrupt);
#endif

#if defined(CONFIG_X86_LOCAL_APIC) && defined(CONFIG_X86_MCE_P4THERMAL)
	/* thermal monitor LVT interrupt */
	alloc_intr_gate(THERMAL_APIC_VECTOR, thermal_interrupt);
#endif

	if (!acpi_ioapic)
		setup_irq(2, &irq2);

	/* setup after call gates are initialised (usually add in
	 * the architecture specific gates)
	 */
	intr_init_hook();

	/*
	 * External FPU? Set up irq13 if so, for
	 * original braindamaged IBM FERR coupling.
	 */
	if (boot_cpu_data.hard_math && !cpu_has_fpu)
		setup_irq(FPU_IRQ, &fpu_irq);

	irq_ctx_init(smp_processor_id());
}
