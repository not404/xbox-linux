#ifndef _ASM_X86_TIMEX_H
#define _ASM_X86_TIMEX_H

#include <asm/processor.h>
#include <asm/tsc.h>

#ifdef CONFIG_X86_XBOX
#  define PIT_TICK_RATE 1125000 /* Xbox has different frequency! */
#else
/* The PIT ticks at this frequency (in HZ): */
#define PIT_TICK_RATE		1193182
#endif /* CONFIG_X86_XBOX */

#define CLOCK_TICK_RATE		PIT_TICK_RATE

#define ARCH_HAS_READ_CURRENT_TIMER

#endif /* _ASM_X86_TIMEX_H */
