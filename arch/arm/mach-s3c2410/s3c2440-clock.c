/* linux/arch/arm/mach-s3c2410/s3c2440-clock.c
 *
 * Copyright (c) 2004-2005 Simtec Electronics
 *	http://armlinux.simtec.co.uk/
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * S3C2440 Clock support
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/sysdev.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/clk.h>

#include <asm/hardware.h>
#include <asm/atomic.h>
#include <asm/irq.h>
#include <asm/io.h>

#include <asm/arch/regs-clock.h>

#include "clock.h"
#include "cpu.h"

/* S3C2440 extended clock support */

static unsigned long s3c2440_camif_upll_round(struct clk *clk,
					      unsigned long rate)
{
	unsigned long parent_rate = clk_get_rate(clk->parent);
	int div;

	if (rate > parent_rate)
		return parent_rate;

	/* note, we remove the +/- 1 calculations for the divisor */

	div = (parent_rate / rate) / 2;

	if (div < 1)
		div = 1;
	else if (div > 16)
		div = 16;

	return parent_rate / (div * 2);
}

static int s3c2440_camif_upll_setrate(struct clk *clk, unsigned long rate)
{
	unsigned long parent_rate = clk_get_rate(clk->parent);
	unsigned long camdivn =  __raw_readl(S3C2440_CAMDIVN);

	rate = s3c2440_camif_upll_round(clk, rate);

	camdivn &= ~(S3C2440_CAMDIVN_CAMCLK_SEL | S3C2440_CAMDIVN_CAMCLK_MASK);

	if (rate != parent_rate) {
		camdivn |= S3C2440_CAMDIVN_CAMCLK_SEL;
		camdivn |= (((parent_rate / rate) / 2) - 1);
	}

	__raw_writel(camdivn, S3C2440_CAMDIVN);

	return 0;
}

/* Extra S3C2440 clocks */

static struct clk s3c2440_clk_cam = {
	.name		= "camif",
	.id		= -1,
	.enable		= s3c2410_clkcon_enable,
	.ctrlbit	= S3C2440_CLKCON_CAMERA,
};

static struct clk s3c2440_clk_cam_upll = {
	.name		= "camif-upll",
	.id		= -1,
	.set_rate	= s3c2440_camif_upll_setrate,
	.round_rate	= s3c2440_camif_upll_round,
};

static struct clk s3c2440_clk_ac97 = {
	.name		= "ac97",
	.id		= -1,
	.enable		= s3c2410_clkcon_enable,
	.ctrlbit	= S3C2440_CLKCON_CAMERA,
};

static int s3c2440_clk_add(struct sys_device *sysdev)
{
	unsigned long camdivn = __raw_readl(S3C2440_CAMDIVN);
	unsigned long clkdivn;
	struct clk *clk_h;
	struct clk *clk_p;
	struct clk *clk_upll;

	printk("S3C2440: Clock Support, DVS %s\n",
	       (camdivn & S3C2440_CAMDIVN_DVSEN) ? "on" : "off");

	clk_p = clk_get(NULL, "pclk");
	clk_h = clk_get(NULL, "hclk");
	clk_upll = clk_get(NULL, "upll");

	if (IS_ERR(clk_p) || IS_ERR(clk_h) || IS_ERR(clk_upll)) {
		printk(KERN_ERR "S3C2440: Failed to get parent clocks\n");
		return -EINVAL;
	}

	/* check rate of UPLL, and if it is near 96MHz, then change
	 * to using half the UPLL rate for the system */

	if (clk_get_rate(clk_upll) > (94 * MHZ)) {
		clk_usb_bus.rate = clk_get_rate(clk_upll) / 2;

		mutex_lock(&clocks_mutex);

		clkdivn = __raw_readl(S3C2410_CLKDIVN);
		clkdivn |= S3C2440_CLKDIVN_UCLK;
		__raw_writel(clkdivn, S3C2410_CLKDIVN);

		mutex_unlock(&clocks_mutex);
	}

	s3c2440_clk_cam.parent = clk_h;
	s3c2440_clk_ac97.parent = clk_p;
	s3c2440_clk_cam_upll.parent = clk_upll;

	s3c24xx_register_clock(&s3c2440_clk_ac97);
	s3c24xx_register_clock(&s3c2440_clk_cam);
	s3c24xx_register_clock(&s3c2440_clk_cam_upll);

	clk_disable(&s3c2440_clk_ac97);
	clk_disable(&s3c2440_clk_cam);

	return 0;
}

static struct sysdev_driver s3c2440_clk_driver = {
	.add	= s3c2440_clk_add,
};

static __init int s3c24xx_clk_driver(void)
{
	return sysdev_driver_register(&s3c2440_sysclass, &s3c2440_clk_driver);
}

arch_initcall(s3c24xx_clk_driver);
