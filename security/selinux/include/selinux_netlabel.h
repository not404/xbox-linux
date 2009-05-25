/*
 * SELinux interface to the NetLabel subsystem
 *
 * Author : Paul Moore <paul.moore@hp.com>
 *
 */

/*
 * (c) Copyright Hewlett-Packard Development Company, L.P., 2006
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY;  without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;  if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#ifndef _SELINUX_NETLABEL_H_
#define _SELINUX_NETLABEL_H_

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <net/sock.h>

#include "avc.h"
#include "objsec.h"

#ifdef CONFIG_NETLABEL
void selinux_netlbl_cache_invalidate(void);
int selinux_netlbl_socket_post_create(struct socket *sock,
				      int sock_family,
				      u32 sid);
void selinux_netlbl_sock_graft(struct sock *sk, struct socket *sock);
u32 selinux_netlbl_inet_conn_request(struct sk_buff *skb, u32 sock_sid);
int selinux_netlbl_sock_rcv_skb(struct sk_security_struct *sksec,
				struct sk_buff *skb,
				struct avc_audit_data *ad);
u32 selinux_netlbl_socket_getpeersec_stream(struct socket *sock);
u32 selinux_netlbl_socket_getpeersec_dgram(struct sk_buff *skb);
void selinux_netlbl_sk_security_init(struct sk_security_struct *ssec,
				     int family);
void selinux_netlbl_sk_clone_security(struct sk_security_struct *ssec,
				      struct sk_security_struct *newssec);
int selinux_netlbl_inode_permission(struct inode *inode, int mask);
int selinux_netlbl_socket_setsockopt(struct socket *sock,
				     int level,
				     int optname);
#else
static inline void selinux_netlbl_cache_invalidate(void)
{
	return;
}

static inline int selinux_netlbl_socket_post_create(struct socket *sock,
						    int sock_family,
						    u32 sid)
{
	return 0;
}

static inline void selinux_netlbl_sock_graft(struct sock *sk,
					     struct socket *sock)
{
	return;
}

static inline u32 selinux_netlbl_inet_conn_request(struct sk_buff *skb,
						   u32 sock_sid)
{
	return SECSID_NULL;
}

static inline int selinux_netlbl_sock_rcv_skb(struct sk_security_struct *sksec,
					      struct sk_buff *skb,
					      struct avc_audit_data *ad)
{
	return 0;
}

static inline u32 selinux_netlbl_socket_getpeersec_stream(struct socket *sock)
{
	return SECSID_NULL;
}

static inline u32 selinux_netlbl_socket_getpeersec_dgram(struct sk_buff *skb)
{
	return SECSID_NULL;
}

static inline void selinux_netlbl_sk_security_init(
	                                       struct sk_security_struct *ssec,
					       int family)
{
	return;
}

static inline void selinux_netlbl_sk_clone_security(
	                                   struct sk_security_struct *ssec,
					   struct sk_security_struct *newssec)
{
	return;
}

static inline int selinux_netlbl_inode_permission(struct inode *inode,
						  int mask)
{
	return 0;
}

static inline int selinux_netlbl_socket_setsockopt(struct socket *sock,
						   int level,
						   int optname)
{
	return 0;
}
#endif /* CONFIG_NETLABEL */

#endif
