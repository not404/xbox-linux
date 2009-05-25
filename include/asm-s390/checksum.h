#ifndef _S390_CHECKSUM_H
#define _S390_CHECKSUM_H

/*
 *  include/asm-s390/checksum.h
 *    S390 fast network checksum routines
 *    see also arch/S390/lib/checksum.c
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Ulrich Hild        (first version)
 *               Martin Schwidefsky (heavily optimized CKSM version)
 *               D.J. Barrow        (third attempt) 
 */

#include <asm/uaccess.h>

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
static inline unsigned int
csum_partial(const unsigned char * buff, int len, unsigned int sum)
{
	register unsigned long reg2 asm("2") = (unsigned long) buff;
	register unsigned long reg3 asm("3") = (unsigned long) len;

	asm volatile(
		"0:	cksm	%0,%1\n"	/* do checksum on longs */
		"	jo	0b\n"
		: "+d" (sum), "+d" (reg2), "+d" (reg3) : : "cc", "memory");
	return sum;
}

/*
 * the same as csum_partial_copy, but copies from user space.
 *
 * here even more important to align src and dst on a 32-bit (or even
 * better 64-bit) boundary
 *
 * Copy from userspace and compute checksum.  If we catch an exception
 * then zero the rest of the buffer.
 */
static inline unsigned int
csum_partial_copy_from_user(const char __user *src, char *dst,
                                          int len, unsigned int sum,
                                          int *err_ptr)
{
	int missing;

	missing = copy_from_user(dst, src, len);
	if (missing) {
		memset(dst + len - missing, 0, missing);
		*err_ptr = -EFAULT;
	}
		
	return csum_partial(dst, len, sum);
}


static inline unsigned int
csum_partial_copy_nocheck (const char *src, char *dst, int len, unsigned int sum)
{
        memcpy(dst,src,len);
	return csum_partial(dst, len, sum);
}

/*
 *      Fold a partial checksum without adding pseudo headers
 */
static inline unsigned short
csum_fold(unsigned int sum)
{
#ifndef __s390x__
	register_pair rp;

	asm volatile(
		"	slr	%N1,%N1\n"	/* %0 = H L */
		"	lr	%1,%0\n"	/* %0 = H L, %1 = H L 0 0 */
		"	srdl	%1,16\n"	/* %0 = H L, %1 = 0 H L 0 */
		"	alr	%1,%N1\n"	/* %0 = H L, %1 = L H L 0 */
		"	alr	%0,%1\n"	/* %0 = H+L+C L+H */
		"	srl	%0,16\n"	/* %0 = H+L+C */
		: "+&d" (sum), "=d" (rp) : : "cc");
#else /* __s390x__ */
	asm volatile(
		"	sr	3,3\n"		/* %0 = H*65536 + L */
		"	lr	2,%0\n"		/* %0 = H L, 2/3 = H L / 0 0 */
		"	srdl	2,16\n"		/* %0 = H L, 2/3 = 0 H / L 0 */
		"	alr	2,3\n"		/* %0 = H L, 2/3 = L H / L 0 */
		"	alr	%0,2\n"		/* %0 = H+L+C L+H */
		"	srl	%0,16\n"	/* %0 = H+L+C */
		: "+&d" (sum) : : "cc", "2", "3");
#endif /* __s390x__ */
	return ((unsigned short) ~sum);
}

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 *
 */
static inline unsigned short
ip_fast_csum(unsigned char *iph, unsigned int ihl)
{
	return csum_fold(csum_partial(iph, ihl*4, 0));
}

/*
 * computes the checksum of the TCP/UDP pseudo-header
 * returns a 32-bit checksum
 */
static inline unsigned int 
csum_tcpudp_nofold(unsigned long saddr, unsigned long daddr,
                   unsigned short len, unsigned short proto,
                   unsigned int sum)
{
#ifndef __s390x__
	asm volatile(
		"	alr	%0,%1\n" /* sum += saddr */
		"	brc	12,0f\n"
		"	ahi	%0,1\n"  /* add carry */
		"0:"
		: "+&d" (sum) : "d" (saddr) : "cc");
	asm volatile(
		"	alr	%0,%1\n" /* sum += daddr */
		"	brc	12,1f\n"
		"	ahi	%0,1\n"  /* add carry */
		"1:"
		: "+&d" (sum) : "d" (daddr) : "cc");
	asm volatile(
		"	alr	%0,%1\n" /* sum += (len<<16) + (proto<<8) */
		"	brc	12,2f\n"
		"	ahi	%0,1\n"  /* add carry */
		"2:"
		: "+&d" (sum)
		: "d" (((unsigned int) len<<16) + (unsigned int) proto)
		: "cc");
#else /* __s390x__ */
	asm volatile(
		"	lgfr	%0,%0\n"
		"	algr	%0,%1\n"  /* sum += saddr */
		"	brc	12,0f\n"
		"	aghi	%0,1\n"   /* add carry */
		"0:	algr	%0,%2\n"  /* sum += daddr */
		"	brc	12,1f\n"
		"	aghi	%0,1\n"   /* add carry */
		"1:	algfr	%0,%3\n"  /* sum += (len<<16) + proto */
		"	brc	12,2f\n"
		"	aghi	%0,1\n"   /* add carry */
		"2:	srlg	0,%0,32\n"
		"	alr	%0,0\n"   /* fold to 32 bits */
		"	brc	12,3f\n"
		"	ahi	%0,1\n"   /* add carry */
		"3:	llgfr	%0,%0"
		: "+&d" (sum)
		: "d" (saddr), "d" (daddr),
		  "d" (((unsigned int) len<<16) + (unsigned int) proto)
		: "cc", "0");
#endif /* __s390x__ */
	return sum;
}

/*
 * computes the checksum of the TCP/UDP pseudo-header
 * returns a 16-bit checksum, already complemented
 */

static inline unsigned short int
csum_tcpudp_magic(unsigned long saddr, unsigned long daddr,
                  unsigned short len, unsigned short proto,
                  unsigned int sum)
{
	return csum_fold(csum_tcpudp_nofold(saddr,daddr,len,proto,sum));
}

/*
 * this routine is used for miscellaneous IP-like checksums, mainly
 * in icmp.c
 */

static inline unsigned short
ip_compute_csum(unsigned char * buff, int len)
{
	return csum_fold(csum_partial(buff, len, 0));
}

#endif /* _S390_CHECKSUM_H */


