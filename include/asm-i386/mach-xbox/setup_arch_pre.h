#ifndef _SETUP_PRE_XBOX_
#define _SETUP_PRE_XBOX_

/* Hook to call BIOS initialisation function */

/* no action for generic */

#define ARCH_SETUP arch_setup_xbox();

#include <linux/timex.h>
#include <asm/io.h>

//int CLOCK_TICK_RATE;
int machine_is_xbox __initdata = 0;

static  inline void arch_setup_xbox(void) {
	outl(0x80000000, 0xcf8);
	if (inl(0xcfc)==0x02a510de) { /* Xbox PCI 0:0:0 ID 0x10de/0x02a5 */
		machine_is_xbox = 1;
//		CLOCK_TICK_RATE = 1125000;
	} else {
//		CLOCK_TICK_RATE = 1193180;
	}
}

#endif
