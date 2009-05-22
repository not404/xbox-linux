/*
 *  arch/s390/kernel/cpcmd.c
 *
 *  S390 version
 *    Copyright (C) 1999,2005 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com),
 *               Christian Borntraeger (cborntra@de.ibm.com),
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <asm/ebcdic.h>
#include <asm/cpcmd.h>
#include <asm/system.h>

static DEFINE_SPINLOCK(cpcmd_lock);
static char cpcmd_buf[241];

/*
 * the caller of __cpcmd has to ensure that the response buffer is below 2 GB
 */
int  __cpcmd(const char *cmd, char *response, int rlen, int *response_code)
{
	unsigned long flags, cmdlen;
	int return_code, return_len;

	spin_lock_irqsave(&cpcmd_lock, flags);
	cmdlen = strlen(cmd);
	BUG_ON(cmdlen > 240);
	memcpy(cpcmd_buf, cmd, cmdlen);
	ASCEBC(cpcmd_buf, cmdlen);

	if (response != NULL && rlen > 0) {
		register unsigned long reg2 asm ("2") = (addr_t) cpcmd_buf;
		register unsigned long reg3 asm ("3") = (addr_t) response;
		register unsigned long reg4 asm ("4") = cmdlen | 0x40000000L;
		register unsigned long reg5 asm ("5") = rlen;

		memset(response, 0, rlen);
		asm volatile(
#ifndef CONFIG_64BIT
			"	diag	%2,%0,0x8\n"
			"	brc	8,1f\n"
			"	ar	%1,%4\n"
#else /* CONFIG_64BIT */
			"	sam31\n"
			"	diag	%2,%0,0x8\n"
			"	sam64\n"
			"	brc	8,1f\n"
			"	agr	%1,%4\n"
#endif /* CONFIG_64BIT */
			"1:\n"
			: "+d" (reg4), "+d" (reg5)
			: "d" (reg2), "d" (reg3), "d" (rlen) : "cc");
		return_code = (int) reg4;
		return_len = (int) reg5;
                EBCASC(response, rlen);
        } else {
		register unsigned long reg2 asm ("2") = (addr_t) cpcmd_buf;
		register unsigned long reg3 asm ("3") = cmdlen;
		return_len = 0;
		asm volatile(
#ifndef CONFIG_64BIT
			"	diag	%1,%0,0x8\n"
#else /* CONFIG_64BIT */
			"	sam31\n"
			"	diag	%1,%0,0x8\n"
			"	sam64\n"
#endif /* CONFIG_64BIT */
			: "+d" (reg3) : "d" (reg2) : "cc");
		return_code = (int) reg3;
        }
	spin_unlock_irqrestore(&cpcmd_lock, flags);
	if (response_code != NULL)
		*response_code = return_code;
	return return_len;
}

EXPORT_SYMBOL(__cpcmd);

#ifdef CONFIG_64BIT
int cpcmd(const char *cmd, char *response, int rlen, int *response_code)
{
	char *lowbuf;
	int len;

	if ((rlen == 0) || (response == NULL)
	    || !((unsigned long)response >> 31))
		len = __cpcmd(cmd, response, rlen, response_code);
	else {
		lowbuf = kmalloc(rlen, GFP_KERNEL | GFP_DMA);
		if (!lowbuf) {
			printk(KERN_WARNING
				"cpcmd: could not allocate response buffer\n");
			return -ENOMEM;
		}
		len = __cpcmd(cmd, lowbuf, rlen, response_code);
		memcpy(response, lowbuf, rlen);
		kfree(lowbuf);
	}
	return len;
}

EXPORT_SYMBOL(cpcmd);
#endif		/* CONFIG_64BIT */
