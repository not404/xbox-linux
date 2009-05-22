/*
 * Copyright (C) 2004-2006 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_AVR32_CHECKSUM_H
#define __ASM_AVR32_CHECKSUM_H

/*
 * computes the checksum of a memory block at buff, length len,
 * and adds in "sum" (32-bit)
 *
 * returns a 32-bit number suitable for feeding into itself
 * or csum_tcpudp_magic
 *
 * this function must be called with even lengths, except
 * for the last fragment, which may be odd
 *
 * it's best to have buff aligned on a 32-bit boundary
 */
unsigned int csum_partial(const unsigned char * buff, int len,
			  unsigned int sum);

/*
 * the same as csum_partial, but copies from src while it
 * checksums, and handles user-space pointer exceptions correctly, when needed.
 *
 * here even more important to align src and dst on a 32-bit (or even
 * better 64-bit) boundary
 */
unsigned int csum_partial_copy_generic(const char *src, char *dst, int len,
				       int sum, int *src_err_ptr,
				       int *dst_err_ptr);

/*
 *	Note: when you get a NULL pointer exception here this means someone
 *	passed in an incorrect kernel address to one of these functions.
 *
 *	If you use these functions directly please don't forget the
 *	verify_area().
 */
static inline
unsigned int csum_partial_copy_nocheck(const char *src, char *dst,
				       int len, int sum)
{
	return csum_partial_copy_generic(src, dst, len, sum, NULL, NULL);
}

static inline
unsigned int csum_partial_copy_from_user (const char __user *src, char *dst,
					  int len, int sum, int *err_ptr)
{
	return csum_partial_copy_generic((const char __force *)src, dst, len,
					 sum, err_ptr, NULL);
}

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 */
static inline unsigned short ip_fast_csum(unsigned char *iph,
					  unsigned int ihl)
{
	unsigned int sum, tmp;

	__asm__ __volatile__(
		"	ld.w	%0, %1++\n"
		"	ld.w	%3, %1++\n"
		"	sub	%2, 4\n"
		"	add	%0, %3\n"
		"	ld.w	%3, %1++\n"
		"	adc	%0, %0, %3\n"
		"	ld.w	%3, %1++\n"
		"	adc	%0, %0, %3\n"
		"	acr	%0\n"
		"1:	ld.w	%3, %1++\n"
		"	add	%0, %3\n"
		"	acr	%0\n"
		"	sub	%2, 1\n"
		"	brne	1b\n"
		"	lsl	%3, %0, 16\n"
		"	andl	%0, 0\n"
		"	mov	%2, 0xffff\n"
		"	add	%0, %3\n"
		"	adc	%0, %0, %2\n"
		"	com	%0\n"
		"	lsr	%0, 16\n"
		: "=r"(sum), "=r"(iph), "=r"(ihl), "=r"(tmp)
		: "1"(iph), "2"(ihl)
		: "memory", "cc");
	return sum;
}

/*
 *	Fold a partial checksum
 */

static inline unsigned int csum_fold(unsigned int sum)
{
	unsigned int tmp;

	asm("	bfextu	%1, %0, 0, 16\n"
	    "	lsr	%0, 16\n"
	    "	add	%0, %1\n"
	    "	bfextu	%1, %0, 16, 16\n"
	    "	add	%0, %1"
	    : "=&r"(sum), "=&r"(tmp)
	    : "0"(sum));

	return ~sum;
}

static inline unsigned long csum_tcpudp_nofold(unsigned long saddr,
					       unsigned long daddr,
					       unsigned short len,
					       unsigned short proto,
					       unsigned int sum)
{
	asm("	add	%0, %1\n"
	    "	adc	%0, %0, %2\n"
	    "	adc	%0, %0, %3\n"
	    "	acr	%0"
	    : "=r"(sum)
	    : "r"(daddr), "r"(saddr), "r"(ntohs(len) | (proto << 16)),
	      "0"(sum)
	    : "cc");

	return sum;
}

/*
 * computes the checksum of the TCP/UDP pseudo-header
 * returns a 16-bit checksum, already complemented
 */
static inline unsigned short int csum_tcpudp_magic(unsigned long saddr,
						   unsigned long daddr,
						   unsigned short len,
						   unsigned short proto,
						   unsigned int sum)
{
	return csum_fold(csum_tcpudp_nofold(saddr,daddr,len,proto,sum));
}

/*
 * this routine is used for miscellaneous IP-like checksums, mainly
 * in icmp.c
 */

static inline unsigned short ip_compute_csum(unsigned char * buff, int len)
{
    return csum_fold(csum_partial(buff, len, 0));
}

#endif /* __ASM_AVR32_CHECKSUM_H */
