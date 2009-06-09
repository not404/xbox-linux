#ifndef P54_H
#define P54_H

/*
 * Shared defines for all mac80211 Prism54 code
 *
 * Copyright (c) 2006, Michael Wu <flamingice@sourmilk.net>
 *
 * Based on the islsm (softmac prism54) driver, which is:
 * Copyright 2004-2006 Jean-Baptiste Note <jbnote@gmail.com>, et al.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

enum control_frame_types {
	P54_CONTROL_TYPE_FILTER_SET = 0,
	P54_CONTROL_TYPE_CHANNEL_CHANGE,
	P54_CONTROL_TYPE_FREQDONE,
	P54_CONTROL_TYPE_DCFINIT,
	P54_CONTROL_TYPE_ENCRYPTION,
	P54_CONTROL_TYPE_TIM,
	P54_CONTROL_TYPE_POWERMGT,
	P54_CONTROL_TYPE_FREEQUEUE,
	P54_CONTROL_TYPE_TXDONE,
	P54_CONTROL_TYPE_PING,
	P54_CONTROL_TYPE_STAT_READBACK,
	P54_CONTROL_TYPE_BBP,
	P54_CONTROL_TYPE_EEPROM_READBACK,
	P54_CONTROL_TYPE_LED,
	P54_CONTROL_TYPE_GPIO,
	P54_CONTROL_TYPE_TIMER,
	P54_CONTROL_TYPE_MODULATION,
	P54_CONTROL_TYPE_SYNTH_CONFIG,
	P54_CONTROL_TYPE_DETECTOR_VALUE,
	P54_CONTROL_TYPE_XBOW_SYNTH_CFG,
	P54_CONTROL_TYPE_CCE_QUIET,
	P54_CONTROL_TYPE_PSM_STA_UNLOCK,
};

struct p54_control_hdr {
	__le16 magic1;
	__le16 len;
	__le32 req_id;
	__le16 type;	/* enum control_frame_types */
	u8 retry1;
	u8 retry2;
	u8 data[0];
} __attribute__ ((packed));

#define EEPROM_READBACK_LEN 0x3fc

#define ISL38XX_DEV_FIRMWARE_ADDR 0x20000

#define FW_FMAC 0x464d4143
#define FW_LM86 0x4c4d3836
#define FW_LM87 0x4c4d3837
#define FW_LM20 0x4c4d3230

struct p54_common {
	u32 rx_start;
	u32 rx_end;
	struct sk_buff_head tx_queue;
	void (*tx)(struct ieee80211_hw *dev, struct p54_control_hdr *data,
		   size_t len, int free_on_tx);
	int (*open)(struct ieee80211_hw *dev);
	void (*stop)(struct ieee80211_hw *dev);
	int mode;
	u16 seqno;
	u16 rx_mtu;
	u8 headroom;
	u8 tailroom;
	struct mutex conf_mutex;
	u8 mac_addr[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	__le16 filter_type;
	struct pda_iq_autocal_entry *iq_autocal;
	unsigned int iq_autocal_len;
	struct pda_channel_output_limit *output_limit;
	unsigned int output_limit_len;
	struct pda_pa_curve_data *curve_data;
	unsigned int filter_flags;
	u16 rxhw;
	u8 version;
	u8 rx_antenna;
	unsigned int tx_hdr_len;
	void *cached_vdcf;
	unsigned int fw_var;
	unsigned int fw_interface;
	unsigned int output_power;
	u32 tsf_low32;
	u32 tsf_high32;
	struct ieee80211_tx_queue_stats tx_stats[8];
	struct ieee80211_low_level_stats stats;
	struct timer_list stats_timer;
	struct completion stats_comp;
	void *cached_stats;
	int noise;
	void *eeprom;
	struct completion eeprom_comp;
};

int p54_rx(struct ieee80211_hw *dev, struct sk_buff *skb);
int p54_parse_firmware(struct ieee80211_hw *dev, const struct firmware *fw);
int p54_read_eeprom(struct ieee80211_hw *dev);
struct ieee80211_hw *p54_init_common(size_t priv_data_len);
void p54_free_common(struct ieee80211_hw *dev);

#endif /* P54_H */
