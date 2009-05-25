/*
 * linux/arch/arm/mach-at91rm9200/generic.h
 *
 *  Copyright (C) 2005 David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

 /* Processors */
extern void __init at91rm9200_initialize(unsigned long main_clock, unsigned short banks);

 /* Interrupts */
extern void __init at91rm9200_init_interrupts(unsigned int priority[]);
extern void __init at91_aic_init(unsigned int priority[]);

 /* Timer */
struct sys_timer;
extern struct sys_timer at91rm9200_timer;

 /* Clocks */
extern int __init at91_clock_init(unsigned long main_clock);
struct device;
extern void __init at91_clock_associate(const char *id, struct device *dev, const char *func);

 /* Power Management */
extern void at91_irq_suspend(void);
extern void at91_irq_resume(void);

 /* GPIO */
#define AT91RM9200_PQFP		3	/* AT91RM9200 PQFP package has 3 banks */
#define AT91RM9200_BGA		4	/* AT91RM9200 BGA package has 4 banks */

struct at91_gpio_bank {
	unsigned short id;		/* peripheral ID */
	unsigned long offset;		/* offset from system peripheral base */
	struct clk *clock;		/* associated clock */
};
extern void __init at91_gpio_init(struct at91_gpio_bank *, int nr_banks);
extern void __init at91_gpio_irq_setup(void);
