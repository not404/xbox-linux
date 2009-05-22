/*
 * I/O string operations
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *    Copyright (C) 2006 IBM Corporation
 *
 * Largely rewritten by Cort Dougan (cort@cs.nmt.edu)
 * and Paul Mackerras.
 *
 * Adapted for iSeries by Mike Corrigan (mikejc@us.ibm.com)
 * PPC64 updates by Dave Engebretsen (engebret@us.ibm.com)
 *
 * Rewritten in C by Stephen Rothwell.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/module.h>

#include <asm/io.h>
#include <asm/firmware.h>
#include <asm/bug.h>

void _insb(volatile u8 __iomem *port, void *buf, long count)
{
	u8 *tbuf = buf;
	u8 tmp;

	BUG_ON(firmware_has_feature(FW_FEATURE_ISERIES));

	if (unlikely(count <= 0))
		return;
	asm volatile("sync");
	do {
		tmp = *port;
		asm volatile("eieio");
		*tbuf++ = tmp;
	} while (--count != 0);
	asm volatile("twi 0,%0,0; isync" : : "r" (tmp));
}
EXPORT_SYMBOL(_insb);

void _outsb(volatile u8 __iomem *port, const void *buf, long count)
{
	const u8 *tbuf = buf;

	BUG_ON(firmware_has_feature(FW_FEATURE_ISERIES));

	if (unlikely(count <= 0))
		return;
	asm volatile("sync");
	do {
		*port = *tbuf++;
	} while (--count != 0);
	asm volatile("sync");
}
EXPORT_SYMBOL(_outsb);

void _insw_ns(volatile u16 __iomem *port, void *buf, long count)
{
	u16 *tbuf = buf;
	u16 tmp;

	BUG_ON(firmware_has_feature(FW_FEATURE_ISERIES));

	if (unlikely(count <= 0))
		return;
	asm volatile("sync");
	do {
		tmp = *port;
		asm volatile("eieio");
		*tbuf++ = tmp;
	} while (--count != 0);
	asm volatile("twi 0,%0,0; isync" : : "r" (tmp));
}
EXPORT_SYMBOL(_insw_ns);

void _outsw_ns(volatile u16 __iomem *port, const void *buf, long count)
{
	const u16 *tbuf = buf;

	BUG_ON(firmware_has_feature(FW_FEATURE_ISERIES));

	if (unlikely(count <= 0))
		return;
	asm volatile("sync");
	do {
		*port = *tbuf++;
	} while (--count != 0);
	asm volatile("sync");
}
EXPORT_SYMBOL(_outsw_ns);

void _insl_ns(volatile u32 __iomem *port, void *buf, long count)
{
	u32 *tbuf = buf;
	u32 tmp;

	BUG_ON(firmware_has_feature(FW_FEATURE_ISERIES));

	if (unlikely(count <= 0))
		return;
	asm volatile("sync");
	do {
		tmp = *port;
		asm volatile("eieio");
		*tbuf++ = tmp;
	} while (--count != 0);
	asm volatile("twi 0,%0,0; isync" : : "r" (tmp));
}
EXPORT_SYMBOL(_insl_ns);

void _outsl_ns(volatile u32 __iomem *port, const void *buf, long count)
{
	const u32 *tbuf = buf;

	BUG_ON(firmware_has_feature(FW_FEATURE_ISERIES));

	if (unlikely(count <= 0))
		return;
	asm volatile("sync");
	do {
		*port = *tbuf++;
	} while (--count != 0);
	asm volatile("sync");
}
EXPORT_SYMBOL(_outsl_ns);
