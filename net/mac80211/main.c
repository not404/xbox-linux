/*
 * Copyright 2002-2005, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <net/mac80211.h>
#include <net/ieee80211_radiotap.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/wireless.h>
#include <linux/rtnetlink.h>
#include <linux/bitmap.h>
#include <net/net_namespace.h>
#include <net/cfg80211.h>

#include "ieee80211_i.h"
#include "rate.h"
#include "mesh.h"
#include "wep.h"
#include "wme.h"
#include "aes_ccm.h"
#include "led.h"
#include "cfg.h"
#include "debugfs.h"
#include "debugfs_netdev.h"

/*
 * For seeing transmitted packets on monitor interfaces
 * we have a radiotap header too.
 */
struct ieee80211_tx_status_rtap_hdr {
	struct ieee80211_radiotap_header hdr;
	__le16 tx_flags;
	u8 data_retries;
} __attribute__ ((packed));


/* must be called under mdev tx lock */
void ieee80211_configure_filter(struct ieee80211_local *local)
{
	unsigned int changed_flags;
	unsigned int new_flags = 0;

	if (atomic_read(&local->iff_promiscs))
		new_flags |= FIF_PROMISC_IN_BSS;

	if (atomic_read(&local->iff_allmultis))
		new_flags |= FIF_ALLMULTI;

	if (local->monitors)
		new_flags |= FIF_BCN_PRBRESP_PROMISC;

	if (local->fif_fcsfail)
		new_flags |= FIF_FCSFAIL;

	if (local->fif_plcpfail)
		new_flags |= FIF_PLCPFAIL;

	if (local->fif_control)
		new_flags |= FIF_CONTROL;

	if (local->fif_other_bss)
		new_flags |= FIF_OTHER_BSS;

	changed_flags = local->filter_flags ^ new_flags;

	/* be a bit nasty */
	new_flags |= (1<<31);

	local->ops->configure_filter(local_to_hw(local),
				     changed_flags, &new_flags,
				     local->mdev->mc_count,
				     local->mdev->mc_list);

	WARN_ON(new_flags & (1<<31));

	local->filter_flags = new_flags & ~(1<<31);
}

/* master interface */

static int header_parse_80211(const struct sk_buff *skb, unsigned char *haddr)
{
	memcpy(haddr, skb_mac_header(skb) + 10, ETH_ALEN); /* addr2 */
	return ETH_ALEN;
}

static const struct header_ops ieee80211_header_ops = {
	.create		= eth_header,
	.parse		= header_parse_80211,
	.rebuild	= eth_rebuild_header,
	.cache		= eth_header_cache,
	.cache_update	= eth_header_cache_update,
};

static int ieee80211_master_open(struct net_device *dev)
{
	struct ieee80211_master_priv *mpriv = netdev_priv(dev);
	struct ieee80211_local *local = mpriv->local;
	struct ieee80211_sub_if_data *sdata;
	int res = -EOPNOTSUPP;

	/* we hold the RTNL here so can safely walk the list */
	list_for_each_entry(sdata, &local->interfaces, list) {
		if (netif_running(sdata->dev)) {
			res = 0;
			break;
		}
	}

	if (res)
		return res;

	netif_tx_start_all_queues(local->mdev);

	return 0;
}

static int ieee80211_master_stop(struct net_device *dev)
{
	struct ieee80211_master_priv *mpriv = netdev_priv(dev);
	struct ieee80211_local *local = mpriv->local;
	struct ieee80211_sub_if_data *sdata;

	/* we hold the RTNL here so can safely walk the list */
	list_for_each_entry(sdata, &local->interfaces, list)
		if (netif_running(sdata->dev))
			dev_close(sdata->dev);

	return 0;
}

static void ieee80211_master_set_multicast_list(struct net_device *dev)
{
	struct ieee80211_master_priv *mpriv = netdev_priv(dev);
	struct ieee80211_local *local = mpriv->local;

	ieee80211_configure_filter(local);
}

/* everything else */

int ieee80211_if_config(struct ieee80211_sub_if_data *sdata, u32 changed)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_conf conf;

	if (WARN_ON(!netif_running(sdata->dev)))
		return 0;

	if (WARN_ON(sdata->vif.type == NL80211_IFTYPE_AP_VLAN))
		return -EINVAL;

	if (!local->ops->config_interface)
		return 0;

	memset(&conf, 0, sizeof(conf));
	conf.changed = changed;

	if (sdata->vif.type == NL80211_IFTYPE_STATION ||
	    sdata->vif.type == NL80211_IFTYPE_ADHOC) {
		conf.bssid = sdata->u.sta.bssid;
		conf.ssid = sdata->u.sta.ssid;
		conf.ssid_len = sdata->u.sta.ssid_len;
	} else if (sdata->vif.type == NL80211_IFTYPE_AP) {
		conf.bssid = sdata->dev->dev_addr;
		conf.ssid = sdata->u.ap.ssid;
		conf.ssid_len = sdata->u.ap.ssid_len;
	} else if (ieee80211_vif_is_mesh(&sdata->vif)) {
		u8 zero[ETH_ALEN] = { 0 };
		conf.bssid = zero;
		conf.ssid = zero;
		conf.ssid_len = 0;
	} else {
		WARN_ON(1);
		return -EINVAL;
	}

	if (WARN_ON(!conf.bssid && (changed & IEEE80211_IFCC_BSSID)))
		return -EINVAL;

	if (WARN_ON(!conf.ssid && (changed & IEEE80211_IFCC_SSID)))
		return -EINVAL;

	return local->ops->config_interface(local_to_hw(local),
					    &sdata->vif, &conf);
}

int ieee80211_hw_config(struct ieee80211_local *local)
{
	struct ieee80211_channel *chan;
	int ret = 0;

	if (local->sw_scanning)
		chan = local->scan_channel;
	else
		chan = local->oper_channel;

	local->hw.conf.channel = chan;

	if (!local->hw.conf.power_level)
		local->hw.conf.power_level = chan->max_power;
	else
		local->hw.conf.power_level = min(chan->max_power,
					       local->hw.conf.power_level);

	local->hw.conf.max_antenna_gain = chan->max_antenna_gain;

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	printk(KERN_DEBUG "%s: HW CONFIG: freq=%d\n",
	       wiphy_name(local->hw.wiphy), chan->center_freq);
#endif

	if (local->open_count)
		ret = local->ops->config(local_to_hw(local), &local->hw.conf);

	return ret;
}

/**
 * ieee80211_handle_ht should be used only after legacy configuration
 * has been determined namely band, as ht configuration depends upon
 * the hardware's HT abilities for a _specific_ band.
 */
u32 ieee80211_handle_ht(struct ieee80211_local *local, int enable_ht,
			   struct ieee80211_ht_info *req_ht_cap,
			   struct ieee80211_ht_bss_info *req_bss_cap)
{
	struct ieee80211_conf *conf = &local->hw.conf;
	struct ieee80211_supported_band *sband;
	struct ieee80211_ht_info ht_conf;
	struct ieee80211_ht_bss_info ht_bss_conf;
	u32 changed = 0;
	int i;
	u8 max_tx_streams = IEEE80211_HT_CAP_MAX_STREAMS;
	u8 tx_mcs_set_cap;

	sband = local->hw.wiphy->bands[conf->channel->band];

	memset(&ht_conf, 0, sizeof(struct ieee80211_ht_info));
	memset(&ht_bss_conf, 0, sizeof(struct ieee80211_ht_bss_info));

	/* HT is not supported */
	if (!sband->ht_info.ht_supported) {
		conf->flags &= ~IEEE80211_CONF_SUPPORT_HT_MODE;
		goto out;
	}

	/* disable HT */
	if (!enable_ht) {
		if (conf->flags & IEEE80211_CONF_SUPPORT_HT_MODE)
			changed |= BSS_CHANGED_HT;
		conf->flags &= ~IEEE80211_CONF_SUPPORT_HT_MODE;
		conf->ht_conf.ht_supported = 0;
		goto out;
	}


	if (!(conf->flags & IEEE80211_CONF_SUPPORT_HT_MODE))
		changed |= BSS_CHANGED_HT;

	conf->flags |= IEEE80211_CONF_SUPPORT_HT_MODE;
	ht_conf.ht_supported = 1;

	ht_conf.cap = req_ht_cap->cap & sband->ht_info.cap;
	ht_conf.cap &= ~(IEEE80211_HT_CAP_SM_PS);
	ht_conf.cap |= sband->ht_info.cap & IEEE80211_HT_CAP_SM_PS;
	ht_bss_conf.primary_channel = req_bss_cap->primary_channel;
	ht_bss_conf.bss_cap = req_bss_cap->bss_cap;
	ht_bss_conf.bss_op_mode = req_bss_cap->bss_op_mode;

	ht_conf.ampdu_factor = req_ht_cap->ampdu_factor;
	ht_conf.ampdu_density = req_ht_cap->ampdu_density;

	/* Bits 96-100 */
	tx_mcs_set_cap = sband->ht_info.supp_mcs_set[12];

	/* configure suppoerted Tx MCS according to requested MCS
	 * (based in most cases on Rx capabilities of peer) and self
	 * Tx MCS capabilities (as defined by low level driver HW
	 * Tx capabilities) */
	if (!(tx_mcs_set_cap & IEEE80211_HT_CAP_MCS_TX_DEFINED))
		goto check_changed;

	/* Counting from 0 therfore + 1 */
	if (tx_mcs_set_cap & IEEE80211_HT_CAP_MCS_TX_RX_DIFF)
		max_tx_streams = ((tx_mcs_set_cap &
				IEEE80211_HT_CAP_MCS_TX_STREAMS) >> 2) + 1;

	for (i = 0; i < max_tx_streams; i++)
		ht_conf.supp_mcs_set[i] =
			sband->ht_info.supp_mcs_set[i] &
					req_ht_cap->supp_mcs_set[i];

	if (tx_mcs_set_cap & IEEE80211_HT_CAP_MCS_TX_UEQM)
		for (i = IEEE80211_SUPP_MCS_SET_UEQM;
		     i < IEEE80211_SUPP_MCS_SET_LEN; i++)
			ht_conf.supp_mcs_set[i] =
				sband->ht_info.supp_mcs_set[i] &
					req_ht_cap->supp_mcs_set[i];

check_changed:
	/* if bss configuration changed store the new one */
	if (memcmp(&conf->ht_conf, &ht_conf, sizeof(ht_conf)) ||
	    memcmp(&conf->ht_bss_conf, &ht_bss_conf, sizeof(ht_bss_conf))) {
		changed |= BSS_CHANGED_HT;
		memcpy(&conf->ht_conf, &ht_conf, sizeof(ht_conf));
		memcpy(&conf->ht_bss_conf, &ht_bss_conf, sizeof(ht_bss_conf));
	}
out:
	return changed;
}

void ieee80211_bss_info_change_notify(struct ieee80211_sub_if_data *sdata,
				      u32 changed)
{
	struct ieee80211_local *local = sdata->local;

	if (WARN_ON(sdata->vif.type == NL80211_IFTYPE_AP_VLAN))
		return;

	if (!changed)
		return;

	if (local->ops->bss_info_changed)
		local->ops->bss_info_changed(local_to_hw(local),
					     &sdata->vif,
					     &sdata->bss_conf,
					     changed);
}

u32 ieee80211_reset_erp_info(struct ieee80211_sub_if_data *sdata)
{
	sdata->bss_conf.use_cts_prot = 0;
	sdata->bss_conf.use_short_preamble = 0;
	return BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE;
}

void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw,
				 struct sk_buff *skb)
{
	struct ieee80211_local *local = hw_to_local(hw);
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	int tmp;

	skb->dev = local->mdev;
	skb->pkt_type = IEEE80211_TX_STATUS_MSG;
	skb_queue_tail(info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS ?
		       &local->skb_queue : &local->skb_queue_unreliable, skb);
	tmp = skb_queue_len(&local->skb_queue) +
		skb_queue_len(&local->skb_queue_unreliable);
	while (tmp > IEEE80211_IRQSAFE_QUEUE_LIMIT &&
	       (skb = skb_dequeue(&local->skb_queue_unreliable))) {
		dev_kfree_skb_irq(skb);
		tmp--;
		I802_DEBUG_INC(local->tx_status_drop);
	}
	tasklet_schedule(&local->tasklet);
}
EXPORT_SYMBOL(ieee80211_tx_status_irqsafe);

static void ieee80211_tasklet_handler(unsigned long data)
{
	struct ieee80211_local *local = (struct ieee80211_local *) data;
	struct sk_buff *skb;
	struct ieee80211_rx_status rx_status;
	struct ieee80211_ra_tid *ra_tid;

	while ((skb = skb_dequeue(&local->skb_queue)) ||
	       (skb = skb_dequeue(&local->skb_queue_unreliable))) {
		switch (skb->pkt_type) {
		case IEEE80211_RX_MSG:
			/* status is in skb->cb */
			memcpy(&rx_status, skb->cb, sizeof(rx_status));
			/* Clear skb->pkt_type in order to not confuse kernel
			 * netstack. */
			skb->pkt_type = 0;
			__ieee80211_rx(local_to_hw(local), skb, &rx_status);
			break;
		case IEEE80211_TX_STATUS_MSG:
			skb->pkt_type = 0;
			ieee80211_tx_status(local_to_hw(local), skb);
			break;
		case IEEE80211_DELBA_MSG:
			ra_tid = (struct ieee80211_ra_tid *) &skb->cb;
			ieee80211_stop_tx_ba_cb(local_to_hw(local),
						ra_tid->ra, ra_tid->tid);
			dev_kfree_skb(skb);
			break;
		case IEEE80211_ADDBA_MSG:
			ra_tid = (struct ieee80211_ra_tid *) &skb->cb;
			ieee80211_start_tx_ba_cb(local_to_hw(local),
						 ra_tid->ra, ra_tid->tid);
			dev_kfree_skb(skb);
			break ;
		default:
			WARN_ON(1);
			dev_kfree_skb(skb);
			break;
		}
	}
}

/* Remove added headers (e.g., QoS control), encryption header/MIC, etc. to
 * make a prepared TX frame (one that has been given to hw) to look like brand
 * new IEEE 802.11 frame that is ready to go through TX processing again.
 */
static void ieee80211_remove_tx_extra(struct ieee80211_local *local,
				      struct ieee80211_key *key,
				      struct sk_buff *skb)
{
	unsigned int hdrlen, iv_len, mic_len;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	hdrlen = ieee80211_hdrlen(hdr->frame_control);

	if (!key)
		goto no_key;

	switch (key->conf.alg) {
	case ALG_WEP:
		iv_len = WEP_IV_LEN;
		mic_len = WEP_ICV_LEN;
		break;
	case ALG_TKIP:
		iv_len = TKIP_IV_LEN;
		mic_len = TKIP_ICV_LEN;
		break;
	case ALG_CCMP:
		iv_len = CCMP_HDR_LEN;
		mic_len = CCMP_MIC_LEN;
		break;
	default:
		goto no_key;
	}

	if (skb->len >= hdrlen + mic_len &&
	    !(key->flags & KEY_FLAG_UPLOADED_TO_HARDWARE))
		skb_trim(skb, skb->len - mic_len);
	if (skb->len >= hdrlen + iv_len) {
		memmove(skb->data + iv_len, skb->data, hdrlen);
		hdr = (struct ieee80211_hdr *)skb_pull(skb, iv_len);
	}

no_key:
	if (ieee80211_is_data_qos(hdr->frame_control)) {
		hdr->frame_control &= ~cpu_to_le16(IEEE80211_STYPE_QOS_DATA);
		memmove(skb->data + IEEE80211_QOS_CTL_LEN, skb->data,
			hdrlen - IEEE80211_QOS_CTL_LEN);
		skb_pull(skb, IEEE80211_QOS_CTL_LEN);
	}
}

static void ieee80211_handle_filtered_frame(struct ieee80211_local *local,
					    struct sta_info *sta,
					    struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	sta->tx_filtered_count++;

	/*
	 * Clear the TX filter mask for this STA when sending the next
	 * packet. If the STA went to power save mode, this will happen
	 * when it wakes up for the next time.
	 */
	set_sta_flags(sta, WLAN_STA_CLEAR_PS_FILT);

	/*
	 * This code races in the following way:
	 *
	 *  (1) STA sends frame indicating it will go to sleep and does so
	 *  (2) hardware/firmware adds STA to filter list, passes frame up
	 *  (3) hardware/firmware processes TX fifo and suppresses a frame
	 *  (4) we get TX status before having processed the frame and
	 *	knowing that the STA has gone to sleep.
	 *
	 * This is actually quite unlikely even when both those events are
	 * processed from interrupts coming in quickly after one another or
	 * even at the same time because we queue both TX status events and
	 * RX frames to be processed by a tasklet and process them in the
	 * same order that they were received or TX status last. Hence, there
	 * is no race as long as the frame RX is processed before the next TX
	 * status, which drivers can ensure, see below.
	 *
	 * Note that this can only happen if the hardware or firmware can
	 * actually add STAs to the filter list, if this is done by the
	 * driver in response to set_tim() (which will only reduce the race
	 * this whole filtering tries to solve, not completely solve it)
	 * this situation cannot happen.
	 *
	 * To completely solve this race drivers need to make sure that they
	 *  (a) don't mix the irq-safe/not irq-safe TX status/RX processing
	 *	functions and
	 *  (b) always process RX events before TX status events if ordering
	 *      can be unknown, for example with different interrupt status
	 *	bits.
	 */
	if (test_sta_flags(sta, WLAN_STA_PS) &&
	    skb_queue_len(&sta->tx_filtered) < STA_MAX_TX_BUFFER) {
		ieee80211_remove_tx_extra(local, sta->key, skb);
		skb_queue_tail(&sta->tx_filtered, skb);
		return;
	}

	if (!test_sta_flags(sta, WLAN_STA_PS) &&
	    !(info->flags & IEEE80211_TX_CTL_REQUEUE)) {
		/* Software retry the packet once */
		info->flags |= IEEE80211_TX_CTL_REQUEUE;
		ieee80211_remove_tx_extra(local, sta->key, skb);
		dev_queue_xmit(skb);
		return;
	}

#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	if (net_ratelimit())
		printk(KERN_DEBUG "%s: dropped TX filtered frame, "
		       "queue_len=%d PS=%d @%lu\n",
		       wiphy_name(local->hw.wiphy),
		       skb_queue_len(&sta->tx_filtered),
		       !!test_sta_flags(sta, WLAN_STA_PS), jiffies);
#endif
	dev_kfree_skb(skb);
}

void ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct sk_buff *skb2;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	struct ieee80211_local *local = hw_to_local(hw);
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	u16 frag, type;
	__le16 fc;
	struct ieee80211_supported_band *sband;
	struct ieee80211_tx_status_rtap_hdr *rthdr;
	struct ieee80211_sub_if_data *sdata;
	struct net_device *prev_dev = NULL;
	struct sta_info *sta;

	rcu_read_lock();

	sta = sta_info_get(local, hdr->addr1);

	if (sta) {
		if (info->status.excessive_retries &&
		    test_sta_flags(sta, WLAN_STA_PS)) {
			/*
			 * The STA is in power save mode, so assume
			 * that this TX packet failed because of that.
			 */
			ieee80211_handle_filtered_frame(local, sta, skb);
			rcu_read_unlock();
			return;
		}

		fc = hdr->frame_control;

		if ((info->flags & IEEE80211_TX_STAT_AMPDU_NO_BACK) &&
		    (ieee80211_is_data_qos(fc))) {
			u16 tid, ssn;
			u8 *qc;

			qc = ieee80211_get_qos_ctl(hdr);
			tid = qc[0] & 0xf;
			ssn = ((le16_to_cpu(hdr->seq_ctrl) + 0x10)
						& IEEE80211_SCTL_SEQ);
			ieee80211_send_bar(sta->sdata, hdr->addr1,
					   tid, ssn);
		}

		if (info->flags & IEEE80211_TX_STAT_TX_FILTERED) {
			ieee80211_handle_filtered_frame(local, sta, skb);
			rcu_read_unlock();
			return;
		} else {
			if (info->status.excessive_retries)
				sta->tx_retry_failed++;
			sta->tx_retry_count += info->status.retry_count;
		}

		sband = local->hw.wiphy->bands[info->band];
		rate_control_tx_status(local, sband, sta, skb);
	}

	rcu_read_unlock();

	ieee80211_led_tx(local, 0);

	/* SNMP counters
	 * Fragments are passed to low-level drivers as separate skbs, so these
	 * are actually fragments, not frames. Update frame counters only for
	 * the first fragment of the frame. */

	frag = le16_to_cpu(hdr->seq_ctrl) & IEEE80211_SCTL_FRAG;
	type = le16_to_cpu(hdr->frame_control) & IEEE80211_FCTL_FTYPE;

	if (info->flags & IEEE80211_TX_STAT_ACK) {
		if (frag == 0) {
			local->dot11TransmittedFrameCount++;
			if (is_multicast_ether_addr(hdr->addr1))
				local->dot11MulticastTransmittedFrameCount++;
			if (info->status.retry_count > 0)
				local->dot11RetryCount++;
			if (info->status.retry_count > 1)
				local->dot11MultipleRetryCount++;
		}

		/* This counter shall be incremented for an acknowledged MPDU
		 * with an individual address in the address 1 field or an MPDU
		 * with a multicast address in the address 1 field of type Data
		 * or Management. */
		if (!is_multicast_ether_addr(hdr->addr1) ||
		    type == IEEE80211_FTYPE_DATA ||
		    type == IEEE80211_FTYPE_MGMT)
			local->dot11TransmittedFragmentCount++;
	} else {
		if (frag == 0)
			local->dot11FailedCount++;
	}

	/* this was a transmitted frame, but now we want to reuse it */
	skb_orphan(skb);

	/*
	 * This is a bit racy but we can avoid a lot of work
	 * with this test...
	 */
	if (!local->monitors && !local->cooked_mntrs) {
		dev_kfree_skb(skb);
		return;
	}

	/* send frame to monitor interfaces now */

	if (skb_headroom(skb) < sizeof(*rthdr)) {
		printk(KERN_ERR "ieee80211_tx_status: headroom too small\n");
		dev_kfree_skb(skb);
		return;
	}

	rthdr = (struct ieee80211_tx_status_rtap_hdr *)
				skb_push(skb, sizeof(*rthdr));

	memset(rthdr, 0, sizeof(*rthdr));
	rthdr->hdr.it_len = cpu_to_le16(sizeof(*rthdr));
	rthdr->hdr.it_present =
		cpu_to_le32((1 << IEEE80211_RADIOTAP_TX_FLAGS) |
			    (1 << IEEE80211_RADIOTAP_DATA_RETRIES));

	if (!(info->flags & IEEE80211_TX_STAT_ACK) &&
	    !is_multicast_ether_addr(hdr->addr1))
		rthdr->tx_flags |= cpu_to_le16(IEEE80211_RADIOTAP_F_TX_FAIL);

	if ((info->flags & IEEE80211_TX_CTL_USE_RTS_CTS) &&
	    (info->flags & IEEE80211_TX_CTL_USE_CTS_PROTECT))
		rthdr->tx_flags |= cpu_to_le16(IEEE80211_RADIOTAP_F_TX_CTS);
	else if (info->flags & IEEE80211_TX_CTL_USE_RTS_CTS)
		rthdr->tx_flags |= cpu_to_le16(IEEE80211_RADIOTAP_F_TX_RTS);

	rthdr->data_retries = info->status.retry_count;

	/* XXX: is this sufficient for BPF? */
	skb_set_mac_header(skb, 0);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_802_2);
	memset(skb->cb, 0, sizeof(skb->cb));

	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		if (sdata->vif.type == NL80211_IFTYPE_MONITOR) {
			if (!netif_running(sdata->dev))
				continue;

			if (prev_dev) {
				skb2 = skb_clone(skb, GFP_ATOMIC);
				if (skb2) {
					skb2->dev = prev_dev;
					netif_rx(skb2);
				}
			}

			prev_dev = sdata->dev;
		}
	}
	if (prev_dev) {
		skb->dev = prev_dev;
		netif_rx(skb);
		skb = NULL;
	}
	rcu_read_unlock();
	dev_kfree_skb(skb);
}
EXPORT_SYMBOL(ieee80211_tx_status);

struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len,
					const struct ieee80211_ops *ops)
{
	struct ieee80211_local *local;
	int priv_size;
	struct wiphy *wiphy;

	/* Ensure 32-byte alignment of our private data and hw private data.
	 * We use the wiphy priv data for both our ieee80211_local and for
	 * the driver's private data
	 *
	 * In memory it'll be like this:
	 *
	 * +-------------------------+
	 * | struct wiphy	    |
	 * +-------------------------+
	 * | struct ieee80211_local  |
	 * +-------------------------+
	 * | driver's private data   |
	 * +-------------------------+
	 *
	 */
	priv_size = ((sizeof(struct ieee80211_local) +
		      NETDEV_ALIGN_CONST) & ~NETDEV_ALIGN_CONST) +
		    priv_data_len;

	wiphy = wiphy_new(&mac80211_config_ops, priv_size);

	if (!wiphy)
		return NULL;

	wiphy->privid = mac80211_wiphy_privid;

	local = wiphy_priv(wiphy);
	local->hw.wiphy = wiphy;

	local->hw.priv = (char *)local +
			 ((sizeof(struct ieee80211_local) +
			   NETDEV_ALIGN_CONST) & ~NETDEV_ALIGN_CONST);

	BUG_ON(!ops->tx);
	BUG_ON(!ops->start);
	BUG_ON(!ops->stop);
	BUG_ON(!ops->config);
	BUG_ON(!ops->add_interface);
	BUG_ON(!ops->remove_interface);
	BUG_ON(!ops->configure_filter);
	local->ops = ops;

	local->hw.queues = 1; /* default */

	local->rts_threshold = IEEE80211_MAX_RTS_THRESHOLD;
	local->fragmentation_threshold = IEEE80211_MAX_FRAG_THRESHOLD;
	local->short_retry_limit = 7;
	local->long_retry_limit = 4;
	local->hw.conf.radio_enabled = 1;

	INIT_LIST_HEAD(&local->interfaces);

	spin_lock_init(&local->key_lock);

	INIT_DELAYED_WORK(&local->scan_work, ieee80211_scan_work);

	sta_info_init(local);

	tasklet_init(&local->tx_pending_tasklet, ieee80211_tx_pending,
		     (unsigned long)local);
	tasklet_disable(&local->tx_pending_tasklet);

	tasklet_init(&local->tasklet,
		     ieee80211_tasklet_handler,
		     (unsigned long) local);
	tasklet_disable(&local->tasklet);

	skb_queue_head_init(&local->skb_queue);
	skb_queue_head_init(&local->skb_queue_unreliable);

	return local_to_hw(local);
}
EXPORT_SYMBOL(ieee80211_alloc_hw);

int ieee80211_register_hw(struct ieee80211_hw *hw)
{
	struct ieee80211_local *local = hw_to_local(hw);
	const char *name;
	int result;
	enum ieee80211_band band;
	struct net_device *mdev;
	struct ieee80211_master_priv *mpriv;

	/*
	 * generic code guarantees at least one band,
	 * set this very early because much code assumes
	 * that hw.conf.channel is assigned
	 */
	for (band = 0; band < IEEE80211_NUM_BANDS; band++) {
		struct ieee80211_supported_band *sband;

		sband = local->hw.wiphy->bands[band];
		if (sband) {
			/* init channel we're on */
			local->hw.conf.channel =
			local->oper_channel =
			local->scan_channel = &sband->channels[0];
			break;
		}
	}

	/* if low-level driver supports AP, we also support VLAN */
	if (local->hw.wiphy->interface_modes & BIT(NL80211_IFTYPE_AP))
		local->hw.wiphy->interface_modes |= BIT(NL80211_IFTYPE_AP_VLAN);

	/* mac80211 always supports monitor */
	local->hw.wiphy->interface_modes |= BIT(NL80211_IFTYPE_MONITOR);

	result = wiphy_register(local->hw.wiphy);
	if (result < 0)
		return result;

	/*
	 * We use the number of queues for feature tests (QoS, HT) internally
	 * so restrict them appropriately.
	 */
	if (hw->queues > IEEE80211_MAX_QUEUES)
		hw->queues = IEEE80211_MAX_QUEUES;
	if (hw->ampdu_queues > IEEE80211_MAX_AMPDU_QUEUES)
		hw->ampdu_queues = IEEE80211_MAX_AMPDU_QUEUES;
	if (hw->queues < 4)
		hw->ampdu_queues = 0;

	mdev = alloc_netdev_mq(sizeof(struct ieee80211_master_priv),
			       "wmaster%d", ether_setup,
			       ieee80211_num_queues(hw));
	if (!mdev)
		goto fail_mdev_alloc;

	mpriv = netdev_priv(mdev);
	mpriv->local = local;
	local->mdev = mdev;

	ieee80211_rx_bss_list_init(local);

	mdev->hard_start_xmit = ieee80211_master_start_xmit;
	mdev->open = ieee80211_master_open;
	mdev->stop = ieee80211_master_stop;
	mdev->type = ARPHRD_IEEE80211;
	mdev->header_ops = &ieee80211_header_ops;
	mdev->set_multicast_list = ieee80211_master_set_multicast_list;

	name = wiphy_dev(local->hw.wiphy)->driver->name;
	local->hw.workqueue = create_freezeable_workqueue(name);
	if (!local->hw.workqueue) {
		result = -ENOMEM;
		goto fail_workqueue;
	}

	/*
	 * The hardware needs headroom for sending the frame,
	 * and we need some headroom for passing the frame to monitor
	 * interfaces, but never both at the same time.
	 */
	local->tx_headroom = max_t(unsigned int , local->hw.extra_tx_headroom,
				   sizeof(struct ieee80211_tx_status_rtap_hdr));

	debugfs_hw_add(local);

	if (local->hw.conf.beacon_int < 10)
		local->hw.conf.beacon_int = 100;

	if (local->hw.max_listen_interval == 0)
		local->hw.max_listen_interval = 1;

	local->hw.conf.listen_interval = local->hw.max_listen_interval;

	local->wstats_flags |= local->hw.flags & (IEEE80211_HW_SIGNAL_UNSPEC |
						  IEEE80211_HW_SIGNAL_DB |
						  IEEE80211_HW_SIGNAL_DBM) ?
			       IW_QUAL_QUAL_UPDATED : IW_QUAL_QUAL_INVALID;
	local->wstats_flags |= local->hw.flags & IEEE80211_HW_NOISE_DBM ?
			       IW_QUAL_NOISE_UPDATED : IW_QUAL_NOISE_INVALID;
	if (local->hw.flags & IEEE80211_HW_SIGNAL_DBM)
		local->wstats_flags |= IW_QUAL_DBM;

	result = sta_info_start(local);
	if (result < 0)
		goto fail_sta_info;

	rtnl_lock();
	result = dev_alloc_name(local->mdev, local->mdev->name);
	if (result < 0)
		goto fail_dev;

	memcpy(local->mdev->dev_addr, local->hw.wiphy->perm_addr, ETH_ALEN);
	SET_NETDEV_DEV(local->mdev, wiphy_dev(local->hw.wiphy));

	result = register_netdevice(local->mdev);
	if (result < 0)
		goto fail_dev;

	result = ieee80211_init_rate_ctrl_alg(local,
					      hw->rate_control_algorithm);
	if (result < 0) {
		printk(KERN_DEBUG "%s: Failed to initialize rate control "
		       "algorithm\n", wiphy_name(local->hw.wiphy));
		goto fail_rate;
	}

	result = ieee80211_wep_init(local);

	if (result < 0) {
		printk(KERN_DEBUG "%s: Failed to initialize wep: %d\n",
		       wiphy_name(local->hw.wiphy), result);
		goto fail_wep;
	}

	local->mdev->select_queue = ieee80211_select_queue;

	/* add one default STA interface */
	result = ieee80211_if_add(local, "wlan%d", NULL,
				  NL80211_IFTYPE_STATION, NULL);
	if (result)
		printk(KERN_WARNING "%s: Failed to add default virtual iface\n",
		       wiphy_name(local->hw.wiphy));

	rtnl_unlock();

	ieee80211_led_init(local);

	return 0;

fail_wep:
	rate_control_deinitialize(local);
fail_rate:
	unregister_netdevice(local->mdev);
	local->mdev = NULL;
fail_dev:
	rtnl_unlock();
	sta_info_stop(local);
fail_sta_info:
	debugfs_hw_del(local);
	destroy_workqueue(local->hw.workqueue);
fail_workqueue:
	if (local->mdev)
		free_netdev(local->mdev);
fail_mdev_alloc:
	wiphy_unregister(local->hw.wiphy);
	return result;
}
EXPORT_SYMBOL(ieee80211_register_hw);

void ieee80211_unregister_hw(struct ieee80211_hw *hw)
{
	struct ieee80211_local *local = hw_to_local(hw);

	tasklet_kill(&local->tx_pending_tasklet);
	tasklet_kill(&local->tasklet);

	rtnl_lock();

	/*
	 * At this point, interface list manipulations are fine
	 * because the driver cannot be handing us frames any
	 * more and the tasklet is killed.
	 */

	/* First, we remove all virtual interfaces. */
	ieee80211_remove_interfaces(local);

	/* then, finally, remove the master interface */
	unregister_netdevice(local->mdev);

	rtnl_unlock();

	ieee80211_rx_bss_list_deinit(local);
	ieee80211_clear_tx_pending(local);
	sta_info_stop(local);
	rate_control_deinitialize(local);
	debugfs_hw_del(local);

	if (skb_queue_len(&local->skb_queue)
			|| skb_queue_len(&local->skb_queue_unreliable))
		printk(KERN_WARNING "%s: skb_queue not empty\n",
		       wiphy_name(local->hw.wiphy));
	skb_queue_purge(&local->skb_queue);
	skb_queue_purge(&local->skb_queue_unreliable);

	destroy_workqueue(local->hw.workqueue);
	wiphy_unregister(local->hw.wiphy);
	ieee80211_wep_free(local);
	ieee80211_led_exit(local);
	free_netdev(local->mdev);
}
EXPORT_SYMBOL(ieee80211_unregister_hw);

void ieee80211_free_hw(struct ieee80211_hw *hw)
{
	struct ieee80211_local *local = hw_to_local(hw);

	wiphy_free(local->hw.wiphy);
}
EXPORT_SYMBOL(ieee80211_free_hw);

static int __init ieee80211_init(void)
{
	struct sk_buff *skb;
	int ret;

	BUILD_BUG_ON(sizeof(struct ieee80211_tx_info) > sizeof(skb->cb));
	BUILD_BUG_ON(offsetof(struct ieee80211_tx_info, driver_data) +
	             IEEE80211_TX_INFO_DRIVER_DATA_SIZE > sizeof(skb->cb));

	ret = rc80211_minstrel_init();
	if (ret)
		return ret;

	ret = rc80211_pid_init();
	if (ret)
		return ret;

	ieee80211_debugfs_netdev_init();

	return 0;
}

static void __exit ieee80211_exit(void)
{
	rc80211_pid_exit();
	rc80211_minstrel_exit();

	/*
	 * For key todo, it'll be empty by now but the work
	 * might still be scheduled.
	 */
	flush_scheduled_work();

	if (mesh_allocated)
		ieee80211s_stop();

	ieee80211_debugfs_netdev_exit();
}


subsys_initcall(ieee80211_init);
module_exit(ieee80211_exit);

MODULE_DESCRIPTION("IEEE 802.11 subsystem");
MODULE_LICENSE("GPL");
