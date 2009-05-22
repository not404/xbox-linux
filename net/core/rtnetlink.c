/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Routing netlink socket interface: protocol independent part.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Fixes:
 *	Vitaly E. Lavrov		RTA_OK arithmetics was wrong.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/capability.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/mutex.h>
#include <linux/if_addr.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/string.h>

#include <linux/inet.h>
#include <linux/netdevice.h>
#include <net/ip.h>
#include <net/protocol.h>
#include <net/arp.h>
#include <net/route.h>
#include <net/udp.h>
#include <net/sock.h>
#include <net/pkt_sched.h>
#include <net/fib_rules.h>
#include <net/netlink.h>
#ifdef CONFIG_NET_WIRELESS_RTNETLINK
#include <linux/wireless.h>
#include <net/iw_handler.h>
#endif	/* CONFIG_NET_WIRELESS_RTNETLINK */

static DEFINE_MUTEX(rtnl_mutex);
static struct sock *rtnl;

void rtnl_lock(void)
{
	mutex_lock(&rtnl_mutex);
}

void __rtnl_unlock(void)
{
	mutex_unlock(&rtnl_mutex);
}

void rtnl_unlock(void)
{
	mutex_unlock(&rtnl_mutex);
	if (rtnl && rtnl->sk_receive_queue.qlen)
		rtnl->sk_data_ready(rtnl, 0);
	netdev_run_todo();
}

int rtnl_trylock(void)
{
	return mutex_trylock(&rtnl_mutex);
}

int rtattr_parse(struct rtattr *tb[], int maxattr, struct rtattr *rta, int len)
{
	memset(tb, 0, sizeof(struct rtattr*)*maxattr);

	while (RTA_OK(rta, len)) {
		unsigned flavor = rta->rta_type;
		if (flavor && flavor <= maxattr)
			tb[flavor-1] = rta;
		rta = RTA_NEXT(rta, len);
	}
	return 0;
}

struct rtnetlink_link * rtnetlink_links[NPROTO];

static const int rtm_min[RTM_NR_FAMILIES] =
{
	[RTM_FAM(RTM_NEWLINK)]      = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
	[RTM_FAM(RTM_NEWADDR)]      = NLMSG_LENGTH(sizeof(struct ifaddrmsg)),
	[RTM_FAM(RTM_NEWROUTE)]     = NLMSG_LENGTH(sizeof(struct rtmsg)),
	[RTM_FAM(RTM_NEWRULE)]      = NLMSG_LENGTH(sizeof(struct fib_rule_hdr)),
	[RTM_FAM(RTM_NEWQDISC)]     = NLMSG_LENGTH(sizeof(struct tcmsg)),
	[RTM_FAM(RTM_NEWTCLASS)]    = NLMSG_LENGTH(sizeof(struct tcmsg)),
	[RTM_FAM(RTM_NEWTFILTER)]   = NLMSG_LENGTH(sizeof(struct tcmsg)),
	[RTM_FAM(RTM_NEWACTION)]    = NLMSG_LENGTH(sizeof(struct tcamsg)),
	[RTM_FAM(RTM_NEWPREFIX)]    = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
	[RTM_FAM(RTM_GETMULTICAST)] = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
	[RTM_FAM(RTM_GETANYCAST)]   = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
};

static const int rta_max[RTM_NR_FAMILIES] =
{
	[RTM_FAM(RTM_NEWLINK)]      = IFLA_MAX,
	[RTM_FAM(RTM_NEWADDR)]      = IFA_MAX,
	[RTM_FAM(RTM_NEWROUTE)]     = RTA_MAX,
	[RTM_FAM(RTM_NEWRULE)]      = FRA_MAX,
	[RTM_FAM(RTM_NEWQDISC)]     = TCA_MAX,
	[RTM_FAM(RTM_NEWTCLASS)]    = TCA_MAX,
	[RTM_FAM(RTM_NEWTFILTER)]   = TCA_MAX,
	[RTM_FAM(RTM_NEWACTION)]    = TCAA_MAX,
};

void __rta_fill(struct sk_buff *skb, int attrtype, int attrlen, const void *data)
{
	struct rtattr *rta;
	int size = RTA_LENGTH(attrlen);

	rta = (struct rtattr*)skb_put(skb, RTA_ALIGN(size));
	rta->rta_type = attrtype;
	rta->rta_len = size;
	memcpy(RTA_DATA(rta), data, attrlen);
	memset(RTA_DATA(rta) + attrlen, 0, RTA_ALIGN(size) - size);
}

size_t rtattr_strlcpy(char *dest, const struct rtattr *rta, size_t size)
{
	size_t ret = RTA_PAYLOAD(rta);
	char *src = RTA_DATA(rta);

	if (ret > 0 && src[ret - 1] == '\0')
		ret--;
	if (size > 0) {
		size_t len = (ret >= size) ? size - 1 : ret;
		memset(dest, 0, size);
		memcpy(dest, src, len);
	}
	return ret;
}

int rtnetlink_send(struct sk_buff *skb, u32 pid, unsigned group, int echo)
{
	int err = 0;

	NETLINK_CB(skb).dst_group = group;
	if (echo)
		atomic_inc(&skb->users);
	netlink_broadcast(rtnl, skb, pid, group, GFP_KERNEL);
	if (echo)
		err = netlink_unicast(rtnl, skb, pid, MSG_DONTWAIT);
	return err;
}

int rtnl_unicast(struct sk_buff *skb, u32 pid)
{
	return nlmsg_unicast(rtnl, skb, pid);
}

int rtnl_notify(struct sk_buff *skb, u32 pid, u32 group,
		struct nlmsghdr *nlh, gfp_t flags)
{
	int report = 0;

	if (nlh)
		report = nlmsg_report(nlh);

	return nlmsg_notify(rtnl, skb, pid, group, report, flags);
}

void rtnl_set_sk_err(u32 group, int error)
{
	netlink_set_err(rtnl, 0, group, error);
}

int rtnetlink_put_metrics(struct sk_buff *skb, u32 *metrics)
{
	struct nlattr *mx;
	int i, valid = 0;

	mx = nla_nest_start(skb, RTA_METRICS);
	if (mx == NULL)
		return -ENOBUFS;

	for (i = 0; i < RTAX_MAX; i++) {
		if (metrics[i]) {
			valid++;
			NLA_PUT_U32(skb, i+1, metrics[i]);
		}
	}

	if (!valid) {
		nla_nest_cancel(skb, mx);
		return 0;
	}

	return nla_nest_end(skb, mx);

nla_put_failure:
	return nla_nest_cancel(skb, mx);
}


static void set_operstate(struct net_device *dev, unsigned char transition)
{
	unsigned char operstate = dev->operstate;

	switch(transition) {
	case IF_OPER_UP:
		if ((operstate == IF_OPER_DORMANT ||
		     operstate == IF_OPER_UNKNOWN) &&
		    !netif_dormant(dev))
			operstate = IF_OPER_UP;
		break;

	case IF_OPER_DORMANT:
		if (operstate == IF_OPER_UP ||
		    operstate == IF_OPER_UNKNOWN)
			operstate = IF_OPER_DORMANT;
		break;
	};

	if (dev->operstate != operstate) {
		write_lock_bh(&dev_base_lock);
		dev->operstate = operstate;
		write_unlock_bh(&dev_base_lock);
		netdev_state_change(dev);
	}
}

static void copy_rtnl_link_stats(struct rtnl_link_stats *a,
				 struct net_device_stats *b)
{
	a->rx_packets = b->rx_packets;
	a->tx_packets = b->tx_packets;
	a->rx_bytes = b->rx_bytes;
	a->tx_bytes = b->tx_bytes;
	a->rx_errors = b->rx_errors;
	a->tx_errors = b->tx_errors;
	a->rx_dropped = b->rx_dropped;
	a->tx_dropped = b->tx_dropped;

	a->multicast = b->multicast;
	a->collisions = b->collisions;

	a->rx_length_errors = b->rx_length_errors;
	a->rx_over_errors = b->rx_over_errors;
	a->rx_crc_errors = b->rx_crc_errors;
	a->rx_frame_errors = b->rx_frame_errors;
	a->rx_fifo_errors = b->rx_fifo_errors;
	a->rx_missed_errors = b->rx_missed_errors;

	a->tx_aborted_errors = b->tx_aborted_errors;
	a->tx_carrier_errors = b->tx_carrier_errors;
	a->tx_fifo_errors = b->tx_fifo_errors;
	a->tx_heartbeat_errors = b->tx_heartbeat_errors;
	a->tx_window_errors = b->tx_window_errors;

	a->rx_compressed = b->rx_compressed;
	a->tx_compressed = b->tx_compressed;
};

static int rtnl_fill_ifinfo(struct sk_buff *skb, struct net_device *dev,
			    void *iwbuf, int iwbuflen, int type, u32 pid,
			    u32 seq, u32 change, unsigned int flags)
{
	struct ifinfomsg *ifm;
	struct nlmsghdr *nlh;

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*ifm), flags);
	if (nlh == NULL)
		return -ENOBUFS;

	ifm = nlmsg_data(nlh);
	ifm->ifi_family = AF_UNSPEC;
	ifm->__ifi_pad = 0;
	ifm->ifi_type = dev->type;
	ifm->ifi_index = dev->ifindex;
	ifm->ifi_flags = dev_get_flags(dev);
	ifm->ifi_change = change;

	NLA_PUT_STRING(skb, IFLA_IFNAME, dev->name);
	NLA_PUT_U32(skb, IFLA_TXQLEN, dev->tx_queue_len);
	NLA_PUT_U32(skb, IFLA_WEIGHT, dev->weight);
	NLA_PUT_U8(skb, IFLA_OPERSTATE,
		   netif_running(dev) ? dev->operstate : IF_OPER_DOWN);
	NLA_PUT_U8(skb, IFLA_LINKMODE, dev->link_mode);
	NLA_PUT_U32(skb, IFLA_MTU, dev->mtu);

	if (dev->ifindex != dev->iflink)
		NLA_PUT_U32(skb, IFLA_LINK, dev->iflink);

	if (dev->master)
		NLA_PUT_U32(skb, IFLA_MASTER, dev->master->ifindex);

	if (dev->qdisc_sleeping)
		NLA_PUT_STRING(skb, IFLA_QDISC, dev->qdisc_sleeping->ops->id);

	if (1) {
		struct rtnl_link_ifmap map = {
			.mem_start   = dev->mem_start,
			.mem_end     = dev->mem_end,
			.base_addr   = dev->base_addr,
			.irq         = dev->irq,
			.dma         = dev->dma,
			.port        = dev->if_port,
		};
		NLA_PUT(skb, IFLA_MAP, sizeof(map), &map);
	}

	if (dev->addr_len) {
		NLA_PUT(skb, IFLA_ADDRESS, dev->addr_len, dev->dev_addr);
		NLA_PUT(skb, IFLA_BROADCAST, dev->addr_len, dev->broadcast);
	}

	if (dev->get_stats) {
		struct net_device_stats *stats = dev->get_stats(dev);
		if (stats) {
			struct nlattr *attr;

			attr = nla_reserve(skb, IFLA_STATS,
					   sizeof(struct rtnl_link_stats));
			if (attr == NULL)
				goto nla_put_failure;

			copy_rtnl_link_stats(nla_data(attr), stats);
		}
	}

	if (iwbuf)
		NLA_PUT(skb, IFLA_WIRELESS, iwbuflen, iwbuf);

	return nlmsg_end(skb, nlh);

nla_put_failure:
	return nlmsg_cancel(skb, nlh);
}

static int rtnl_dump_ifinfo(struct sk_buff *skb, struct netlink_callback *cb)
{
	int idx;
	int s_idx = cb->args[0];
	struct net_device *dev;

	read_lock(&dev_base_lock);
	for (dev=dev_base, idx=0; dev; dev = dev->next, idx++) {
		if (idx < s_idx)
			continue;
		if (rtnl_fill_ifinfo(skb, dev, NULL, 0, RTM_NEWLINK,
				     NETLINK_CB(cb->skb).pid,
				     cb->nlh->nlmsg_seq, 0, NLM_F_MULTI) <= 0)
			break;
	}
	read_unlock(&dev_base_lock);
	cb->args[0] = idx;

	return skb->len;
}

static struct nla_policy ifla_policy[IFLA_MAX+1] __read_mostly = {
	[IFLA_IFNAME]		= { .type = NLA_STRING, .len = IFNAMSIZ-1 },
	[IFLA_MAP]		= { .len = sizeof(struct rtnl_link_ifmap) },
	[IFLA_MTU]		= { .type = NLA_U32 },
	[IFLA_TXQLEN]		= { .type = NLA_U32 },
	[IFLA_WEIGHT]		= { .type = NLA_U32 },
	[IFLA_OPERSTATE]	= { .type = NLA_U8 },
	[IFLA_LINKMODE]		= { .type = NLA_U8 },
};

static int rtnl_setlink(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct ifinfomsg *ifm;
	struct net_device *dev;
	int err, send_addr_notify = 0, modified = 0;
	struct nlattr *tb[IFLA_MAX+1];
	char ifname[IFNAMSIZ];

	err = nlmsg_parse(nlh, sizeof(*ifm), tb, IFLA_MAX, ifla_policy);
	if (err < 0)
		goto errout;

	if (tb[IFLA_IFNAME])
		nla_strlcpy(ifname, tb[IFLA_IFNAME], IFNAMSIZ);
	else
		ifname[0] = '\0';

	err = -EINVAL;
	ifm = nlmsg_data(nlh);
	if (ifm->ifi_index >= 0)
		dev = dev_get_by_index(ifm->ifi_index);
	else if (tb[IFLA_IFNAME])
		dev = dev_get_by_name(ifname);
	else
		goto errout;

	if (dev == NULL) {
		err = -ENODEV;
		goto errout;
	}

	if (tb[IFLA_ADDRESS] &&
	    nla_len(tb[IFLA_ADDRESS]) < dev->addr_len)
		goto errout_dev;

	if (tb[IFLA_BROADCAST] &&
	    nla_len(tb[IFLA_BROADCAST]) < dev->addr_len)
		goto errout_dev;

	if (tb[IFLA_MAP]) {
		struct rtnl_link_ifmap *u_map;
		struct ifmap k_map;

		if (!dev->set_config) {
			err = -EOPNOTSUPP;
			goto errout_dev;
		}

		if (!netif_device_present(dev)) {
			err = -ENODEV;
			goto errout_dev;
		}

		u_map = nla_data(tb[IFLA_MAP]);
		k_map.mem_start = (unsigned long) u_map->mem_start;
		k_map.mem_end = (unsigned long) u_map->mem_end;
		k_map.base_addr = (unsigned short) u_map->base_addr;
		k_map.irq = (unsigned char) u_map->irq;
		k_map.dma = (unsigned char) u_map->dma;
		k_map.port = (unsigned char) u_map->port;

		err = dev->set_config(dev, &k_map);
		if (err < 0)
			goto errout_dev;

		modified = 1;
	}

	if (tb[IFLA_ADDRESS]) {
		struct sockaddr *sa;
		int len;

		if (!dev->set_mac_address) {
			err = -EOPNOTSUPP;
			goto errout_dev;
		}

		if (!netif_device_present(dev)) {
			err = -ENODEV;
			goto errout_dev;
		}

		len = sizeof(sa_family_t) + dev->addr_len;
		sa = kmalloc(len, GFP_KERNEL);
		if (!sa) {
			err = -ENOMEM;
			goto errout_dev;
		}
		sa->sa_family = dev->type;
		memcpy(sa->sa_data, nla_data(tb[IFLA_ADDRESS]),
		       dev->addr_len);
		err = dev->set_mac_address(dev, sa);
		kfree(sa);
		if (err)
			goto errout_dev;
		send_addr_notify = 1;
		modified = 1;
	}

	if (tb[IFLA_MTU]) {
		err = dev_set_mtu(dev, nla_get_u32(tb[IFLA_MTU]));
		if (err < 0)
			goto errout_dev;
		modified = 1;
	}

	/*
	 * Interface selected by interface index but interface
	 * name provided implies that a name change has been
	 * requested.
	 */
	if (ifm->ifi_index >= 0 && ifname[0]) {
		err = dev_change_name(dev, ifname);
		if (err < 0)
			goto errout_dev;
		modified = 1;
	}

#ifdef CONFIG_NET_WIRELESS_RTNETLINK
	if (tb[IFLA_WIRELESS]) {
		/* Call Wireless Extensions.
		 * Various stuff checked in there... */
		err = wireless_rtnetlink_set(dev, nla_data(tb[IFLA_WIRELESS]),
					     nla_len(tb[IFLA_WIRELESS]));
		if (err < 0)
			goto errout_dev;
	}
#endif	/* CONFIG_NET_WIRELESS_RTNETLINK */

	if (tb[IFLA_BROADCAST]) {
		nla_memcpy(dev->broadcast, tb[IFLA_BROADCAST], dev->addr_len);
		send_addr_notify = 1;
	}


	if (ifm->ifi_flags)
		dev_change_flags(dev, ifm->ifi_flags);

	if (tb[IFLA_TXQLEN])
		dev->tx_queue_len = nla_get_u32(tb[IFLA_TXQLEN]);

	if (tb[IFLA_WEIGHT])
		dev->weight = nla_get_u32(tb[IFLA_WEIGHT]);

	if (tb[IFLA_OPERSTATE])
		set_operstate(dev, nla_get_u8(tb[IFLA_OPERSTATE]));

	if (tb[IFLA_LINKMODE]) {
		write_lock_bh(&dev_base_lock);
		dev->link_mode = nla_get_u8(tb[IFLA_LINKMODE]);
		write_unlock_bh(&dev_base_lock);
	}

	err = 0;

errout_dev:
	if (err < 0 && modified && net_ratelimit())
		printk(KERN_WARNING "A link change request failed with "
		       "some changes comitted already. Interface %s may "
		       "have been left with an inconsistent configuration, "
		       "please check.\n", dev->name);

	if (send_addr_notify)
		call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);

	dev_put(dev);
errout:
	return err;
}

static int rtnl_getlink(struct sk_buff *skb, struct nlmsghdr* nlh, void *arg)
{
	struct ifinfomsg *ifm;
	struct nlattr *tb[IFLA_MAX+1];
	struct net_device *dev = NULL;
	struct sk_buff *nskb;
	char *iw_buf = NULL, *iw = NULL;
	int iw_buf_len = 0;
	int err, payload;

	err = nlmsg_parse(nlh, sizeof(*ifm), tb, IFLA_MAX, ifla_policy);
	if (err < 0)
		return err;

	ifm = nlmsg_data(nlh);
	if (ifm->ifi_index >= 0) {
		dev = dev_get_by_index(ifm->ifi_index);
		if (dev == NULL)
			return -ENODEV;
	} else
		return -EINVAL;


#ifdef CONFIG_NET_WIRELESS_RTNETLINK
	if (tb[IFLA_WIRELESS]) {
		/* Call Wireless Extensions. We need to know the size before
		 * we can alloc. Various stuff checked in there... */
		err = wireless_rtnetlink_get(dev, nla_data(tb[IFLA_WIRELESS]),
					     nla_len(tb[IFLA_WIRELESS]),
					     &iw_buf, &iw_buf_len);
		if (err < 0)
			goto errout;

		iw += IW_EV_POINT_OFF;
	}
#endif	/* CONFIG_NET_WIRELESS_RTNETLINK */

	payload = NLMSG_ALIGN(sizeof(struct ifinfomsg) +
			      nla_total_size(iw_buf_len));
	nskb = nlmsg_new(nlmsg_total_size(payload), GFP_KERNEL);
	if (nskb == NULL) {
		err = -ENOBUFS;
		goto errout;
	}

	err = rtnl_fill_ifinfo(nskb, dev, iw, iw_buf_len, RTM_NEWLINK,
			       NETLINK_CB(skb).pid, nlh->nlmsg_seq, 0, 0);
	if (err <= 0) {
		kfree_skb(nskb);
		goto errout;
	}

	err = rtnl_unicast(nskb, NETLINK_CB(skb).pid);
errout:
	kfree(iw_buf);
	dev_put(dev);

	return err;
}

static int rtnl_dump_all(struct sk_buff *skb, struct netlink_callback *cb)
{
	int idx;
	int s_idx = cb->family;

	if (s_idx == 0)
		s_idx = 1;
	for (idx=1; idx<NPROTO; idx++) {
		int type = cb->nlh->nlmsg_type-RTM_BASE;
		if (idx < s_idx || idx == PF_PACKET)
			continue;
		if (rtnetlink_links[idx] == NULL ||
		    rtnetlink_links[idx][type].dumpit == NULL)
			continue;
		if (idx > s_idx)
			memset(&cb->args[0], 0, sizeof(cb->args));
		if (rtnetlink_links[idx][type].dumpit(skb, cb))
			break;
	}
	cb->family = idx;

	return skb->len;
}

void rtmsg_ifinfo(int type, struct net_device *dev, unsigned change)
{
	struct sk_buff *skb;
	int err = -ENOBUFS;

	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL)
		goto errout;

	err = rtnl_fill_ifinfo(skb, dev, NULL, 0, type, 0, 0, change, 0);
	if (err < 0) {
		kfree_skb(skb);
		goto errout;
	}

	err = rtnl_notify(skb, 0, RTNLGRP_LINK, NULL, GFP_KERNEL);
errout:
	if (err < 0)
		rtnl_set_sk_err(RTNLGRP_LINK, err);
}

/* Protected by RTNL sempahore.  */
static struct rtattr **rta_buf;
static int rtattr_max;

/* Process one rtnetlink message. */

static __inline__ int
rtnetlink_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh, int *errp)
{
	struct rtnetlink_link *link;
	struct rtnetlink_link *link_tab;
	int sz_idx, kind;
	int min_len;
	int family;
	int type;
	int err;

	/* Only requests are handled by kernel now */
	if (!(nlh->nlmsg_flags&NLM_F_REQUEST))
		return 0;

	type = nlh->nlmsg_type;

	/* A control message: ignore them */
	if (type < RTM_BASE)
		return 0;

	/* Unknown message: reply with EINVAL */
	if (type > RTM_MAX)
		goto err_inval;

	type -= RTM_BASE;

	/* All the messages must have at least 1 byte length */
	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtgenmsg)))
		return 0;

	family = ((struct rtgenmsg*)NLMSG_DATA(nlh))->rtgen_family;
	if (family >= NPROTO) {
		*errp = -EAFNOSUPPORT;
		return -1;
	}

	link_tab = rtnetlink_links[family];
	if (link_tab == NULL)
		link_tab = rtnetlink_links[PF_UNSPEC];
	link = &link_tab[type];

	sz_idx = type>>2;
	kind = type&3;

	if (kind != 2 && security_netlink_recv(skb, CAP_NET_ADMIN)) {
		*errp = -EPERM;
		return -1;
	}

	if (kind == 2 && nlh->nlmsg_flags&NLM_F_DUMP) {
		if (link->dumpit == NULL)
			link = &(rtnetlink_links[PF_UNSPEC][type]);

		if (link->dumpit == NULL)
			goto err_inval;

		if ((*errp = netlink_dump_start(rtnl, skb, nlh,
						link->dumpit, NULL)) != 0) {
			return -1;
		}

		netlink_queue_skip(nlh, skb);
		return -1;
	}

	memset(rta_buf, 0, (rtattr_max * sizeof(struct rtattr *)));

	min_len = rtm_min[sz_idx];
	if (nlh->nlmsg_len < min_len)
		goto err_inval;

	if (nlh->nlmsg_len > min_len) {
		int attrlen = nlh->nlmsg_len - NLMSG_ALIGN(min_len);
		struct rtattr *attr = (void*)nlh + NLMSG_ALIGN(min_len);

		while (RTA_OK(attr, attrlen)) {
			unsigned flavor = attr->rta_type;
			if (flavor) {
				if (flavor > rta_max[sz_idx])
					goto err_inval;
				rta_buf[flavor-1] = attr;
			}
			attr = RTA_NEXT(attr, attrlen);
		}
	}

	if (link->doit == NULL)
		link = &(rtnetlink_links[PF_UNSPEC][type]);
	if (link->doit == NULL)
		goto err_inval;
	err = link->doit(skb, nlh, (void *)&rta_buf[0]);

	*errp = err;
	return err;

err_inval:
	*errp = -EINVAL;
	return -1;
}

static void rtnetlink_rcv(struct sock *sk, int len)
{
	unsigned int qlen = 0;

	do {
		mutex_lock(&rtnl_mutex);
		netlink_run_queue(sk, &qlen, &rtnetlink_rcv_msg);
		mutex_unlock(&rtnl_mutex);

		netdev_run_todo();
	} while (qlen);
}

static struct rtnetlink_link link_rtnetlink_table[RTM_NR_MSGTYPES] =
{
	[RTM_GETLINK     - RTM_BASE] = { .doit   = rtnl_getlink,
					 .dumpit = rtnl_dump_ifinfo	 },
	[RTM_SETLINK     - RTM_BASE] = { .doit   = rtnl_setlink		 },
	[RTM_GETADDR     - RTM_BASE] = { .dumpit = rtnl_dump_all	 },
	[RTM_GETROUTE    - RTM_BASE] = { .dumpit = rtnl_dump_all	 },
	[RTM_NEWNEIGH    - RTM_BASE] = { .doit   = neigh_add		 },
	[RTM_DELNEIGH    - RTM_BASE] = { .doit   = neigh_delete		 },
	[RTM_GETNEIGH    - RTM_BASE] = { .dumpit = neigh_dump_info	 },
#ifdef CONFIG_FIB_RULES
	[RTM_NEWRULE     - RTM_BASE] = { .doit   = fib_nl_newrule	 },
	[RTM_DELRULE     - RTM_BASE] = { .doit   = fib_nl_delrule	 },
#endif
	[RTM_GETRULE     - RTM_BASE] = { .dumpit = rtnl_dump_all	 },
	[RTM_GETNEIGHTBL - RTM_BASE] = { .dumpit = neightbl_dump_info	 },
	[RTM_SETNEIGHTBL - RTM_BASE] = { .doit   = neightbl_set		 },
};

static int rtnetlink_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct net_device *dev = ptr;
	switch (event) {
	case NETDEV_UNREGISTER:
		rtmsg_ifinfo(RTM_DELLINK, dev, ~0U);
		break;
	case NETDEV_REGISTER:
		rtmsg_ifinfo(RTM_NEWLINK, dev, ~0U);
		break;
	case NETDEV_UP:
	case NETDEV_DOWN:
		rtmsg_ifinfo(RTM_NEWLINK, dev, IFF_UP|IFF_RUNNING);
		break;
	case NETDEV_CHANGE:
	case NETDEV_GOING_DOWN:
		break;
	default:
		rtmsg_ifinfo(RTM_NEWLINK, dev, 0);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block rtnetlink_dev_notifier = {
	.notifier_call	= rtnetlink_event,
};

void __init rtnetlink_init(void)
{
	int i;

	rtattr_max = 0;
	for (i = 0; i < ARRAY_SIZE(rta_max); i++)
		if (rta_max[i] > rtattr_max)
			rtattr_max = rta_max[i];
	rta_buf = kmalloc(rtattr_max * sizeof(struct rtattr *), GFP_KERNEL);
	if (!rta_buf)
		panic("rtnetlink_init: cannot allocate rta_buf\n");

	rtnl = netlink_kernel_create(NETLINK_ROUTE, RTNLGRP_MAX, rtnetlink_rcv,
	                             THIS_MODULE);
	if (rtnl == NULL)
		panic("rtnetlink_init: cannot initialize rtnetlink\n");
	netlink_set_nonroot(NETLINK_ROUTE, NL_NONROOT_RECV);
	register_netdevice_notifier(&rtnetlink_dev_notifier);
	rtnetlink_links[PF_UNSPEC] = link_rtnetlink_table;
	rtnetlink_links[PF_PACKET] = link_rtnetlink_table;
}

EXPORT_SYMBOL(__rta_fill);
EXPORT_SYMBOL(rtattr_strlcpy);
EXPORT_SYMBOL(rtattr_parse);
EXPORT_SYMBOL(rtnetlink_links);
EXPORT_SYMBOL(rtnetlink_put_metrics);
EXPORT_SYMBOL(rtnl_lock);
EXPORT_SYMBOL(rtnl_trylock);
EXPORT_SYMBOL(rtnl_unlock);
EXPORT_SYMBOL(rtnl_unicast);
EXPORT_SYMBOL(rtnl_notify);
EXPORT_SYMBOL(rtnl_set_sk_err);
