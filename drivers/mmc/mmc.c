/*
 *  linux/drivers/mmc/mmc.c
 *
 *  Copyright (C) 2003-2004 Russell King, All Rights Reserved.
 *  SD support Copyright (C) 2004 Ian Molton, All Rights Reserved.
 *  SD support Copyright (C) 2005 Pierre Ossman, All Rights Reserved.
 *  MMCv4 support Copyright (C) 2006 Philip Langdale, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <asm/scatterlist.h>
#include <linux/scatterlist.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/protocol.h>

#include "mmc.h"

#define CMD_RETRIES	3

/*
 * OCR Bit positions to 10s of Vdd mV.
 */
static const unsigned short mmc_ocr_bit_to_vdd[] = {
	150,	155,	160,	165,	170,	180,	190,	200,
	210,	220,	230,	240,	250,	260,	270,	280,
	290,	300,	310,	320,	330,	340,	350,	360
};

static const unsigned int tran_exp[] = {
	10000,		100000,		1000000,	10000000,
	0,		0,		0,		0
};

static const unsigned char tran_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int tacc_exp[] = {
	1,	10,	100,	1000,	10000,	100000,	1000000, 10000000,
};

static const unsigned int tacc_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};


/**
 *	mmc_request_done - finish processing an MMC request
 *	@host: MMC host which completed request
 *	@mrq: MMC request which request
 *
 *	MMC drivers should call this function when they have completed
 *	their processing of a request.
 */
void mmc_request_done(struct mmc_host *host, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	int err = cmd->error;

	pr_debug("%s: req done (CMD%u): %d/%d/%d: %08x %08x %08x %08x\n",
		 mmc_hostname(host), cmd->opcode, err,
		 mrq->data ? mrq->data->error : 0,
		 mrq->stop ? mrq->stop->error : 0,
		 cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);

	if (err && cmd->retries) {
		cmd->retries--;
		cmd->error = 0;
		host->ops->request(host, mrq);
	} else if (mrq->done) {
		mrq->done(mrq);
	}
}

EXPORT_SYMBOL(mmc_request_done);

/**
 *	mmc_start_request - start a command on a host
 *	@host: MMC host to start command on
 *	@mrq: MMC request to start
 *
 *	Queue a command on the specified host.  We expect the
 *	caller to be holding the host lock with interrupts disabled.
 */
void
mmc_start_request(struct mmc_host *host, struct mmc_request *mrq)
{
	pr_debug("%s: starting CMD%u arg %08x flags %08x\n",
		 mmc_hostname(host), mrq->cmd->opcode,
		 mrq->cmd->arg, mrq->cmd->flags);

	WARN_ON(!host->claimed);

	mrq->cmd->error = 0;
	mrq->cmd->mrq = mrq;
	if (mrq->data) {
		BUG_ON(mrq->data->blksz > host->max_blk_size);
		BUG_ON(mrq->data->blocks > host->max_blk_count);
		BUG_ON(mrq->data->blocks * mrq->data->blksz >
			host->max_req_size);

		mrq->cmd->data = mrq->data;
		mrq->data->error = 0;
		mrq->data->mrq = mrq;
		if (mrq->stop) {
			mrq->data->stop = mrq->stop;
			mrq->stop->error = 0;
			mrq->stop->mrq = mrq;
		}
	}
	host->ops->request(host, mrq);
}

EXPORT_SYMBOL(mmc_start_request);

static void mmc_wait_done(struct mmc_request *mrq)
{
	complete(mrq->done_data);
}

int mmc_wait_for_req(struct mmc_host *host, struct mmc_request *mrq)
{
	DECLARE_COMPLETION_ONSTACK(complete);

	mrq->done_data = &complete;
	mrq->done = mmc_wait_done;

	mmc_start_request(host, mrq);

	wait_for_completion(&complete);

	return 0;
}

EXPORT_SYMBOL(mmc_wait_for_req);

/**
 *	mmc_wait_for_cmd - start a command and wait for completion
 *	@host: MMC host to start command
 *	@cmd: MMC command to start
 *	@retries: maximum number of retries
 *
 *	Start a new MMC command for a host, and wait for the command
 *	to complete.  Return any error that occurred while the command
 *	was executing.  Do not attempt to parse the response.
 */
int mmc_wait_for_cmd(struct mmc_host *host, struct mmc_command *cmd, int retries)
{
	struct mmc_request mrq;

	BUG_ON(!host->claimed);

	memset(&mrq, 0, sizeof(struct mmc_request));

	memset(cmd->resp, 0, sizeof(cmd->resp));
	cmd->retries = retries;

	mrq.cmd = cmd;
	cmd->data = NULL;

	mmc_wait_for_req(host, &mrq);

	return cmd->error;
}

EXPORT_SYMBOL(mmc_wait_for_cmd);

/**
 *	mmc_wait_for_app_cmd - start an application command and wait for
 			       completion
 *	@host: MMC host to start command
 *	@rca: RCA to send MMC_APP_CMD to
 *	@cmd: MMC command to start
 *	@retries: maximum number of retries
 *
 *	Sends a MMC_APP_CMD, checks the card response, sends the command
 *	in the parameter and waits for it to complete. Return any error
 *	that occurred while the command was executing.  Do not attempt to
 *	parse the response.
 */
int mmc_wait_for_app_cmd(struct mmc_host *host, unsigned int rca,
	struct mmc_command *cmd, int retries)
{
	struct mmc_request mrq;
	struct mmc_command appcmd;

	int i, err;

	BUG_ON(!host->claimed);
	BUG_ON(retries < 0);

	err = MMC_ERR_INVALID;

	/*
	 * We have to resend MMC_APP_CMD for each attempt so
	 * we cannot use the retries field in mmc_command.
	 */
	for (i = 0;i <= retries;i++) {
		memset(&mrq, 0, sizeof(struct mmc_request));

		appcmd.opcode = MMC_APP_CMD;
		appcmd.arg = rca << 16;
		appcmd.flags = MMC_RSP_R1 | MMC_CMD_AC;
		appcmd.retries = 0;
		memset(appcmd.resp, 0, sizeof(appcmd.resp));
		appcmd.data = NULL;

		mrq.cmd = &appcmd;
		appcmd.data = NULL;

		mmc_wait_for_req(host, &mrq);

		if (appcmd.error) {
			err = appcmd.error;
			continue;
		}

		/* Check that card supported application commands */
		if (!(appcmd.resp[0] & R1_APP_CMD))
			return MMC_ERR_FAILED;

		memset(&mrq, 0, sizeof(struct mmc_request));

		memset(cmd->resp, 0, sizeof(cmd->resp));
		cmd->retries = 0;

		mrq.cmd = cmd;
		cmd->data = NULL;

		mmc_wait_for_req(host, &mrq);

		err = cmd->error;
		if (cmd->error == MMC_ERR_NONE)
			break;
	}

	return err;
}

EXPORT_SYMBOL(mmc_wait_for_app_cmd);

/**
 *	mmc_set_data_timeout - set the timeout for a data command
 *	@data: data phase for command
 *	@card: the MMC card associated with the data transfer
 *	@write: flag to differentiate reads from writes
 */
void mmc_set_data_timeout(struct mmc_data *data, const struct mmc_card *card,
			  int write)
{
	unsigned int mult;

	/*
	 * SD cards use a 100 multiplier rather than 10
	 */
	mult = mmc_card_sd(card) ? 100 : 10;

	/*
	 * Scale up the multiplier (and therefore the timeout) by
	 * the r2w factor for writes.
	 */
	if (write)
		mult <<= card->csd.r2w_factor;

	data->timeout_ns = card->csd.tacc_ns * mult;
	data->timeout_clks = card->csd.tacc_clks * mult;

	/*
	 * SD cards also have an upper limit on the timeout.
	 */
	if (mmc_card_sd(card)) {
		unsigned int timeout_us, limit_us;

		timeout_us = data->timeout_ns / 1000;
		timeout_us += data->timeout_clks * 1000 /
			(card->host->ios.clock / 1000);

		if (write)
			limit_us = 250000;
		else
			limit_us = 100000;

		/*
		 * SDHC cards always use these fixed values.
		 */
		if (timeout_us > limit_us || mmc_card_blockaddr(card)) {
			data->timeout_ns = limit_us * 1000;
			data->timeout_clks = 0;
		}
	}
}
EXPORT_SYMBOL(mmc_set_data_timeout);

static int mmc_select_card(struct mmc_host *host, struct mmc_card *card);

/**
 *	__mmc_claim_host - exclusively claim a host
 *	@host: mmc host to claim
 *	@card: mmc card to claim host for
 *
 *	Claim a host for a set of operations.  If a valid card
 *	is passed and this wasn't the last card selected, select
 *	the card before returning.
 *
 *	Note: you should use mmc_card_claim_host or mmc_claim_host.
 */
int __mmc_claim_host(struct mmc_host *host, struct mmc_card *card)
{
	DECLARE_WAITQUEUE(wait, current);
	unsigned long flags;
	int err = 0;

	add_wait_queue(&host->wq, &wait);
	spin_lock_irqsave(&host->lock, flags);
	while (1) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!host->claimed)
			break;
		spin_unlock_irqrestore(&host->lock, flags);
		schedule();
		spin_lock_irqsave(&host->lock, flags);
	}
	set_current_state(TASK_RUNNING);
	host->claimed = 1;
	spin_unlock_irqrestore(&host->lock, flags);
	remove_wait_queue(&host->wq, &wait);

	if (card != (void *)-1) {
		err = mmc_select_card(host, card);
		if (err != MMC_ERR_NONE)
			return err;
	}

	return err;
}

EXPORT_SYMBOL(__mmc_claim_host);

/**
 *	mmc_release_host - release a host
 *	@host: mmc host to release
 *
 *	Release a MMC host, allowing others to claim the host
 *	for their operations.
 */
void mmc_release_host(struct mmc_host *host)
{
	unsigned long flags;

	BUG_ON(!host->claimed);

	spin_lock_irqsave(&host->lock, flags);
	host->claimed = 0;
	spin_unlock_irqrestore(&host->lock, flags);

	wake_up(&host->wq);
}

EXPORT_SYMBOL(mmc_release_host);

static inline void mmc_set_ios(struct mmc_host *host)
{
	struct mmc_ios *ios = &host->ios;

	pr_debug("%s: clock %uHz busmode %u powermode %u cs %u Vdd %u "
		"width %u timing %u\n",
		 mmc_hostname(host), ios->clock, ios->bus_mode,
		 ios->power_mode, ios->chip_select, ios->vdd,
		 ios->bus_width, ios->timing);

	host->ops->set_ios(host, ios);
}

static int mmc_select_card(struct mmc_host *host, struct mmc_card *card)
{
	int err;
	struct mmc_command cmd;

	BUG_ON(!host->claimed);

	if (host->card_selected == card)
		return MMC_ERR_NONE;

	host->card_selected = card;

	cmd.opcode = MMC_SELECT_CARD;
	cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
	if (err != MMC_ERR_NONE)
		return err;

	/*
	 * We can only change the bus width of SD cards when
	 * they are selected so we have to put the handling
	 * here.
	 *
	 * The card is in 1 bit mode by default so
	 * we only need to change if it supports the
	 * wider version.
	 */
	if (mmc_card_sd(card) &&
		(card->scr.bus_widths & SD_SCR_BUS_WIDTH_4)) {

		/*
		* Default bus width is 1 bit.
		*/
		host->ios.bus_width = MMC_BUS_WIDTH_1;

		if (host->caps & MMC_CAP_4_BIT_DATA) {
			struct mmc_command cmd;
			cmd.opcode = SD_APP_SET_BUS_WIDTH;
			cmd.arg = SD_BUS_WIDTH_4;
			cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

			err = mmc_wait_for_app_cmd(host, card->rca, &cmd,
				CMD_RETRIES);
			if (err != MMC_ERR_NONE)
				return err;

			host->ios.bus_width = MMC_BUS_WIDTH_4;
		}
	}

	mmc_set_ios(host);

	return MMC_ERR_NONE;
}

/*
 * Ensure that no card is selected.
 */
static void mmc_deselect_cards(struct mmc_host *host)
{
	struct mmc_command cmd;

	if (host->card_selected) {
		host->card_selected = NULL;

		cmd.opcode = MMC_SELECT_CARD;
		cmd.arg = 0;
		cmd.flags = MMC_RSP_NONE | MMC_CMD_AC;

		mmc_wait_for_cmd(host, &cmd, 0);
	}
}


static inline void mmc_delay(unsigned int ms)
{
	if (ms < 1000 / HZ) {
		cond_resched();
		mdelay(ms);
	} else {
		msleep(ms);
	}
}

/*
 * Mask off any voltages we don't support and select
 * the lowest voltage
 */
static u32 mmc_select_voltage(struct mmc_host *host, u32 ocr)
{
	int bit;

	ocr &= host->ocr_avail;

	bit = ffs(ocr);
	if (bit) {
		bit -= 1;

		ocr &= 3 << bit;

		host->ios.vdd = bit;
		mmc_set_ios(host);
	} else {
		ocr = 0;
	}

	return ocr;
}

#define UNSTUFF_BITS(resp,start,size)					\
	({								\
		const int __size = size;				\
		const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1;	\
		const int __off = 3 - ((start) / 32);			\
		const int __shft = (start) & 31;			\
		u32 __res;						\
									\
		__res = resp[__off] >> __shft;				\
		if (__size + __shft > 32)				\
			__res |= resp[__off-1] << ((32 - __shft) % 32);	\
		__res & __mask;						\
	})

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
static void mmc_decode_cid(struct mmc_card *card)
{
	u32 *resp = card->raw_cid;

	memset(&card->cid, 0, sizeof(struct mmc_cid));

	if (mmc_card_sd(card)) {
		/*
		 * SD doesn't currently have a version field so we will
		 * have to assume we can parse this.
		 */
		card->cid.manfid		= UNSTUFF_BITS(resp, 120, 8);
		card->cid.oemid			= UNSTUFF_BITS(resp, 104, 16);
		card->cid.prod_name[0]		= UNSTUFF_BITS(resp, 96, 8);
		card->cid.prod_name[1]		= UNSTUFF_BITS(resp, 88, 8);
		card->cid.prod_name[2]		= UNSTUFF_BITS(resp, 80, 8);
		card->cid.prod_name[3]		= UNSTUFF_BITS(resp, 72, 8);
		card->cid.prod_name[4]		= UNSTUFF_BITS(resp, 64, 8);
		card->cid.hwrev			= UNSTUFF_BITS(resp, 60, 4);
		card->cid.fwrev			= UNSTUFF_BITS(resp, 56, 4);
		card->cid.serial		= UNSTUFF_BITS(resp, 24, 32);
		card->cid.year			= UNSTUFF_BITS(resp, 12, 8);
		card->cid.month			= UNSTUFF_BITS(resp, 8, 4);

		card->cid.year += 2000; /* SD cards year offset */
	} else {
		/*
		 * The selection of the format here is based upon published
		 * specs from sandisk and from what people have reported.
		 */
		switch (card->csd.mmca_vsn) {
		case 0: /* MMC v1.0 - v1.2 */
		case 1: /* MMC v1.4 */
			card->cid.manfid	= UNSTUFF_BITS(resp, 104, 24);
			card->cid.prod_name[0]	= UNSTUFF_BITS(resp, 96, 8);
			card->cid.prod_name[1]	= UNSTUFF_BITS(resp, 88, 8);
			card->cid.prod_name[2]	= UNSTUFF_BITS(resp, 80, 8);
			card->cid.prod_name[3]	= UNSTUFF_BITS(resp, 72, 8);
			card->cid.prod_name[4]	= UNSTUFF_BITS(resp, 64, 8);
			card->cid.prod_name[5]	= UNSTUFF_BITS(resp, 56, 8);
			card->cid.prod_name[6]	= UNSTUFF_BITS(resp, 48, 8);
			card->cid.hwrev		= UNSTUFF_BITS(resp, 44, 4);
			card->cid.fwrev		= UNSTUFF_BITS(resp, 40, 4);
			card->cid.serial	= UNSTUFF_BITS(resp, 16, 24);
			card->cid.month		= UNSTUFF_BITS(resp, 12, 4);
			card->cid.year		= UNSTUFF_BITS(resp, 8, 4) + 1997;
			break;

		case 2: /* MMC v2.0 - v2.2 */
		case 3: /* MMC v3.1 - v3.3 */
		case 4: /* MMC v4 */
			card->cid.manfid	= UNSTUFF_BITS(resp, 120, 8);
			card->cid.oemid		= UNSTUFF_BITS(resp, 104, 16);
			card->cid.prod_name[0]	= UNSTUFF_BITS(resp, 96, 8);
			card->cid.prod_name[1]	= UNSTUFF_BITS(resp, 88, 8);
			card->cid.prod_name[2]	= UNSTUFF_BITS(resp, 80, 8);
			card->cid.prod_name[3]	= UNSTUFF_BITS(resp, 72, 8);
			card->cid.prod_name[4]	= UNSTUFF_BITS(resp, 64, 8);
			card->cid.prod_name[5]	= UNSTUFF_BITS(resp, 56, 8);
			card->cid.serial	= UNSTUFF_BITS(resp, 16, 32);
			card->cid.month		= UNSTUFF_BITS(resp, 12, 4);
			card->cid.year		= UNSTUFF_BITS(resp, 8, 4) + 1997;
			break;

		default:
			printk("%s: card has unknown MMCA version %d\n",
				mmc_hostname(card->host), card->csd.mmca_vsn);
			mmc_card_set_bad(card);
			break;
		}
	}
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static void mmc_decode_csd(struct mmc_card *card)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, csd_struct;
	u32 *resp = card->raw_csd;

	if (mmc_card_sd(card)) {
		csd_struct = UNSTUFF_BITS(resp, 126, 2);

		switch (csd_struct) {
		case 0:
			m = UNSTUFF_BITS(resp, 115, 4);
			e = UNSTUFF_BITS(resp, 112, 3);
			csd->tacc_ns	 = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
			csd->tacc_clks	 = UNSTUFF_BITS(resp, 104, 8) * 100;

			m = UNSTUFF_BITS(resp, 99, 4);
			e = UNSTUFF_BITS(resp, 96, 3);
			csd->max_dtr	  = tran_exp[e] * tran_mant[m];
			csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

			e = UNSTUFF_BITS(resp, 47, 3);
			m = UNSTUFF_BITS(resp, 62, 12);
			csd->capacity	  = (1 + m) << (e + 2);

			csd->read_blkbits = UNSTUFF_BITS(resp, 80, 4);
			csd->read_partial = UNSTUFF_BITS(resp, 79, 1);
			csd->write_misalign = UNSTUFF_BITS(resp, 78, 1);
			csd->read_misalign = UNSTUFF_BITS(resp, 77, 1);
			csd->r2w_factor = UNSTUFF_BITS(resp, 26, 3);
			csd->write_blkbits = UNSTUFF_BITS(resp, 22, 4);
			csd->write_partial = UNSTUFF_BITS(resp, 21, 1);
			break;
		case 1:
			/*
			 * This is a block-addressed SDHC card. Most
			 * interesting fields are unused and have fixed
			 * values. To avoid getting tripped by buggy cards,
			 * we assume those fixed values ourselves.
			 */
			mmc_card_set_blockaddr(card);

			csd->tacc_ns	 = 0; /* Unused */
			csd->tacc_clks	 = 0; /* Unused */

			m = UNSTUFF_BITS(resp, 99, 4);
			e = UNSTUFF_BITS(resp, 96, 3);
			csd->max_dtr	  = tran_exp[e] * tran_mant[m];
			csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

			m = UNSTUFF_BITS(resp, 48, 22);
			csd->capacity     = (1 + m) << 10;

			csd->read_blkbits = 9;
			csd->read_partial = 0;
			csd->write_misalign = 0;
			csd->read_misalign = 0;
			csd->r2w_factor = 4; /* Unused */
			csd->write_blkbits = 9;
			csd->write_partial = 0;
			break;
		default:
			printk("%s: unrecognised CSD structure version %d\n",
				mmc_hostname(card->host), csd_struct);
			mmc_card_set_bad(card);
			return;
		}
	} else {
		/*
		 * We only understand CSD structure v1.1 and v1.2.
		 * v1.2 has extra information in bits 15, 11 and 10.
		 */
		csd_struct = UNSTUFF_BITS(resp, 126, 2);
		if (csd_struct != 1 && csd_struct != 2) {
			printk("%s: unrecognised CSD structure version %d\n",
				mmc_hostname(card->host), csd_struct);
			mmc_card_set_bad(card);
			return;
		}

		csd->mmca_vsn	 = UNSTUFF_BITS(resp, 122, 4);
		m = UNSTUFF_BITS(resp, 115, 4);
		e = UNSTUFF_BITS(resp, 112, 3);
		csd->tacc_ns	 = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
		csd->tacc_clks	 = UNSTUFF_BITS(resp, 104, 8) * 100;

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

		e = UNSTUFF_BITS(resp, 47, 3);
		m = UNSTUFF_BITS(resp, 62, 12);
		csd->capacity	  = (1 + m) << (e + 2);

		csd->read_blkbits = UNSTUFF_BITS(resp, 80, 4);
		csd->read_partial = UNSTUFF_BITS(resp, 79, 1);
		csd->write_misalign = UNSTUFF_BITS(resp, 78, 1);
		csd->read_misalign = UNSTUFF_BITS(resp, 77, 1);
		csd->r2w_factor = UNSTUFF_BITS(resp, 26, 3);
		csd->write_blkbits = UNSTUFF_BITS(resp, 22, 4);
		csd->write_partial = UNSTUFF_BITS(resp, 21, 1);
	}
}

/*
 * Given a 64-bit response, decode to our card SCR structure.
 */
static void mmc_decode_scr(struct mmc_card *card)
{
	struct sd_scr *scr = &card->scr;
	unsigned int scr_struct;
	u32 resp[4];

	BUG_ON(!mmc_card_sd(card));

	resp[3] = card->raw_scr[1];
	resp[2] = card->raw_scr[0];

	scr_struct = UNSTUFF_BITS(resp, 60, 4);
	if (scr_struct != 0) {
		printk("%s: unrecognised SCR structure version %d\n",
			mmc_hostname(card->host), scr_struct);
		mmc_card_set_bad(card);
		return;
	}

	scr->sda_vsn = UNSTUFF_BITS(resp, 56, 4);
	scr->bus_widths = UNSTUFF_BITS(resp, 48, 4);
}

/*
 * Locate a MMC card on this MMC host given a raw CID.
 */
static struct mmc_card *mmc_find_card(struct mmc_host *host, u32 *raw_cid)
{
	struct mmc_card *card;

	list_for_each_entry(card, &host->cards, node) {
		if (memcmp(card->raw_cid, raw_cid, sizeof(card->raw_cid)) == 0)
			return card;
	}
	return NULL;
}

/*
 * Allocate a new MMC card, and assign a unique RCA.
 */
static struct mmc_card *
mmc_alloc_card(struct mmc_host *host, u32 *raw_cid, unsigned int *frca)
{
	struct mmc_card *card, *c;
	unsigned int rca = *frca;

	card = kmalloc(sizeof(struct mmc_card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	mmc_init_card(card, host);
	memcpy(card->raw_cid, raw_cid, sizeof(card->raw_cid));

 again:
	list_for_each_entry(c, &host->cards, node)
		if (c->rca == rca) {
			rca++;
			goto again;
		}

	card->rca = rca;

	*frca = rca;

	return card;
}

/*
 * Tell attached cards to go to IDLE state
 */
static void mmc_idle_cards(struct mmc_host *host)
{
	struct mmc_command cmd;

	host->ios.chip_select = MMC_CS_HIGH;
	mmc_set_ios(host);

	mmc_delay(1);

	cmd.opcode = MMC_GO_IDLE_STATE;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_NONE | MMC_CMD_BC;

	mmc_wait_for_cmd(host, &cmd, 0);

	mmc_delay(1);

	host->ios.chip_select = MMC_CS_DONTCARE;
	mmc_set_ios(host);

	mmc_delay(1);
}

/*
 * Apply power to the MMC stack.  This is a two-stage process.
 * First, we enable power to the card without the clock running.
 * We then wait a bit for the power to stabilise.  Finally,
 * enable the bus drivers and clock to the card.
 *
 * We must _NOT_ enable the clock prior to power stablising.
 *
 * If a host does all the power sequencing itself, ignore the
 * initial MMC_POWER_UP stage.
 */
static void mmc_power_up(struct mmc_host *host)
{
	int bit = fls(host->ocr_avail) - 1;

	host->ios.vdd = bit;
	host->ios.bus_mode = MMC_BUSMODE_OPENDRAIN;
	host->ios.chip_select = MMC_CS_DONTCARE;
	host->ios.power_mode = MMC_POWER_UP;
	host->ios.bus_width = MMC_BUS_WIDTH_1;
	host->ios.timing = MMC_TIMING_LEGACY;
	mmc_set_ios(host);

	mmc_delay(1);

	host->ios.clock = host->f_min;
	host->ios.power_mode = MMC_POWER_ON;
	mmc_set_ios(host);

	mmc_delay(2);
}

static void mmc_power_off(struct mmc_host *host)
{
	host->ios.clock = 0;
	host->ios.vdd = 0;
	host->ios.bus_mode = MMC_BUSMODE_OPENDRAIN;
	host->ios.chip_select = MMC_CS_DONTCARE;
	host->ios.power_mode = MMC_POWER_OFF;
	host->ios.bus_width = MMC_BUS_WIDTH_1;
	host->ios.timing = MMC_TIMING_LEGACY;
	mmc_set_ios(host);
}

static int mmc_send_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd;
	int i, err = 0;

	cmd.opcode = MMC_SEND_OP_COND;
	cmd.arg = ocr;
	cmd.flags = MMC_RSP_R3 | MMC_CMD_BCR;

	for (i = 100; i; i--) {
		err = mmc_wait_for_cmd(host, &cmd, 0);
		if (err != MMC_ERR_NONE)
			break;

		if (cmd.resp[0] & MMC_CARD_BUSY || ocr == 0)
			break;

		err = MMC_ERR_TIMEOUT;

		mmc_delay(10);
	}

	if (rocr)
		*rocr = cmd.resp[0];

	return err;
}

static int mmc_send_app_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd;
	int i, err = 0;

	cmd.opcode = SD_APP_OP_COND;
	cmd.arg = ocr;
	cmd.flags = MMC_RSP_R3 | MMC_CMD_BCR;

	for (i = 100; i; i--) {
		err = mmc_wait_for_app_cmd(host, 0, &cmd, CMD_RETRIES);
		if (err != MMC_ERR_NONE)
			break;

		if (cmd.resp[0] & MMC_CARD_BUSY || ocr == 0)
			break;

		err = MMC_ERR_TIMEOUT;

		mmc_delay(10);
	}

	if (rocr)
		*rocr = cmd.resp[0];

	return err;
}

static int mmc_send_if_cond(struct mmc_host *host, u32 ocr, int *rsd2)
{
	struct mmc_command cmd;
	int err, sd2;
	static const u8 test_pattern = 0xAA;

	/*
	* To support SD 2.0 cards, we must always invoke SD_SEND_IF_COND
	* before SD_APP_OP_COND. This command will harmlessly fail for
	* SD 1.0 cards.
	*/
	cmd.opcode = SD_SEND_IF_COND;
	cmd.arg = ((ocr & 0xFF8000) != 0) << 8 | test_pattern;
	cmd.flags = MMC_RSP_R7 | MMC_CMD_BCR;

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err == MMC_ERR_NONE) {
		if ((cmd.resp[0] & 0xFF) == test_pattern) {
			sd2 = 1;
		} else {
			sd2 = 0;
			err = MMC_ERR_FAILED;
		}
	} else {
		/*
		 * Treat errors as SD 1.0 card.
		 */
		sd2 = 0;
		err = MMC_ERR_NONE;
	}
	if (rsd2)
		*rsd2 = sd2;
	return err;
}

/*
 * Discover cards by requesting their CID.  If this command
 * times out, it is not an error; there are no further cards
 * to be discovered.  Add new cards to the list.
 *
 * Create a mmc_card entry for each discovered card, assigning
 * it an RCA, and save the raw CID for decoding later.
 */
static void mmc_discover_cards(struct mmc_host *host)
{
	struct mmc_card *card;
	unsigned int first_rca = 1, err;

	while (1) {
		struct mmc_command cmd;

		cmd.opcode = MMC_ALL_SEND_CID;
		cmd.arg = 0;
		cmd.flags = MMC_RSP_R2 | MMC_CMD_BCR;

		err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
		if (err == MMC_ERR_TIMEOUT) {
			err = MMC_ERR_NONE;
			break;
		}
		if (err != MMC_ERR_NONE) {
			printk(KERN_ERR "%s: error requesting CID: %d\n",
				mmc_hostname(host), err);
			break;
		}

		card = mmc_find_card(host, cmd.resp);
		if (!card) {
			card = mmc_alloc_card(host, cmd.resp, &first_rca);
			if (IS_ERR(card)) {
				err = PTR_ERR(card);
				break;
			}
			list_add(&card->node, &host->cards);
		}

		card->state &= ~MMC_STATE_DEAD;

		if (host->mode == MMC_MODE_SD) {
			mmc_card_set_sd(card);

			cmd.opcode = SD_SEND_RELATIVE_ADDR;
			cmd.arg = 0;
			cmd.flags = MMC_RSP_R6 | MMC_CMD_BCR;

			err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
			if (err != MMC_ERR_NONE)
				mmc_card_set_dead(card);
			else {
				card->rca = cmd.resp[0] >> 16;

				if (!host->ops->get_ro) {
					printk(KERN_WARNING "%s: host does not "
						"support reading read-only "
						"switch. assuming write-enable.\n",
						mmc_hostname(host));
				} else {
					if (host->ops->get_ro(host))
						mmc_card_set_readonly(card);
				}
			}
		} else {
			cmd.opcode = MMC_SET_RELATIVE_ADDR;
			cmd.arg = card->rca << 16;
			cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

			err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
			if (err != MMC_ERR_NONE)
				mmc_card_set_dead(card);
		}
	}
}

static void mmc_read_csds(struct mmc_host *host)
{
	struct mmc_card *card;

	list_for_each_entry(card, &host->cards, node) {
		struct mmc_command cmd;
		int err;

		if (card->state & (MMC_STATE_DEAD|MMC_STATE_PRESENT))
			continue;

		cmd.opcode = MMC_SEND_CSD;
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_R2 | MMC_CMD_AC;

		err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
		if (err != MMC_ERR_NONE) {
			mmc_card_set_dead(card);
			continue;
		}

		memcpy(card->raw_csd, cmd.resp, sizeof(card->raw_csd));

		mmc_decode_csd(card);
		mmc_decode_cid(card);
	}
}

static void mmc_process_ext_csds(struct mmc_host *host)
{
	int err;
	struct mmc_card *card;

	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;

	struct scatterlist sg;

	/*
	 * As the ext_csd is so large and mostly unused, we don't store the
	 * raw block in mmc_card.
	 */
	u8 *ext_csd;
	ext_csd = kmalloc(512, GFP_KERNEL);
	if (!ext_csd) {
		printk("%s: could not allocate a buffer to receive the ext_csd."
		       "mmc v4 cards will be treated as v3.\n",
			mmc_hostname(host));
		return;
	}

	list_for_each_entry(card, &host->cards, node) {
		if (card->state & (MMC_STATE_DEAD|MMC_STATE_PRESENT))
			continue;
		if (mmc_card_sd(card))
			continue;
		if (card->csd.mmca_vsn < CSD_SPEC_VER_4)
			continue;

		err = mmc_select_card(host, card);
		if (err != MMC_ERR_NONE) {
			mmc_card_set_dead(card);
			continue;
		}

		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = MMC_SEND_EXT_CSD;
		cmd.arg = 0;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		memset(&data, 0, sizeof(struct mmc_data));

		mmc_set_data_timeout(&data, card, 0);

		data.blksz = 512;
		data.blocks = 1;
		data.flags = MMC_DATA_READ;
		data.sg = &sg;
		data.sg_len = 1;

		memset(&mrq, 0, sizeof(struct mmc_request));

		mrq.cmd = &cmd;
		mrq.data = &data;

		sg_init_one(&sg, ext_csd, 512);

		mmc_wait_for_req(host, &mrq);

		if (cmd.error != MMC_ERR_NONE || data.error != MMC_ERR_NONE) {
			printk("%s: unable to read EXT_CSD, performance "
				"might suffer.\n", mmc_hostname(card->host));
			continue;
		}

		switch (ext_csd[EXT_CSD_CARD_TYPE]) {
		case EXT_CSD_CARD_TYPE_52 | EXT_CSD_CARD_TYPE_26:
			card->ext_csd.hs_max_dtr = 52000000;
			break;
		case EXT_CSD_CARD_TYPE_26:
			card->ext_csd.hs_max_dtr = 26000000;
			break;
		default:
			/* MMC v4 spec says this cannot happen */
			printk("%s: card is mmc v4 but doesn't support "
			       "any high-speed modes.\n",
				mmc_hostname(card->host));
			continue;
		}

		if (host->caps & MMC_CAP_MMC_HIGHSPEED) {
			/* Activate highspeed support. */
			cmd.opcode = MMC_SWITCH;
			cmd.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
				  (EXT_CSD_HS_TIMING << 16) |
				  (1 << 8) |
				  EXT_CSD_CMD_SET_NORMAL;
			cmd.flags = MMC_RSP_R1B | MMC_CMD_AC;

			err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
			if (err != MMC_ERR_NONE) {
				printk("%s: failed to switch card to mmc v4 "
				       "high-speed mode.\n",
				       mmc_hostname(card->host));
				continue;
			}

			mmc_card_set_highspeed(card);

			host->ios.timing = MMC_TIMING_SD_HS;
			mmc_set_ios(host);
		}

		/* Check for host support for wide-bus modes. */
		if (host->caps & MMC_CAP_4_BIT_DATA) {
			/* Activate 4-bit support. */
			cmd.opcode = MMC_SWITCH;
			cmd.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
				  (EXT_CSD_BUS_WIDTH << 16) |
				  (EXT_CSD_BUS_WIDTH_4 << 8) |
				  EXT_CSD_CMD_SET_NORMAL;
			cmd.flags = MMC_RSP_R1B | MMC_CMD_AC;

			err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
			if (err != MMC_ERR_NONE) {
				printk("%s: failed to switch card to "
				       "mmc v4 4-bit bus mode.\n",
				       mmc_hostname(card->host));
				continue;
			}

			host->ios.bus_width = MMC_BUS_WIDTH_4;
			mmc_set_ios(host);
		}
	}

	kfree(ext_csd);

	mmc_deselect_cards(host);
}

static void mmc_read_scrs(struct mmc_host *host)
{
	int err;
	struct mmc_card *card;
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;

	list_for_each_entry(card, &host->cards, node) {
		if (card->state & (MMC_STATE_DEAD|MMC_STATE_PRESENT))
			continue;
		if (!mmc_card_sd(card))
			continue;

		err = mmc_select_card(host, card);
		if (err != MMC_ERR_NONE) {
			mmc_card_set_dead(card);
			continue;
		}

		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = MMC_APP_CMD;
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

		err = mmc_wait_for_cmd(host, &cmd, 0);
		if ((err != MMC_ERR_NONE) || !(cmd.resp[0] & R1_APP_CMD)) {
			mmc_card_set_dead(card);
			continue;
		}

		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = SD_APP_SEND_SCR;
		cmd.arg = 0;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		memset(&data, 0, sizeof(struct mmc_data));

		mmc_set_data_timeout(&data, card, 0);

		data.blksz = 1 << 3;
		data.blocks = 1;
		data.flags = MMC_DATA_READ;
		data.sg = &sg;
		data.sg_len = 1;

		memset(&mrq, 0, sizeof(struct mmc_request));

		mrq.cmd = &cmd;
		mrq.data = &data;

		sg_init_one(&sg, (u8*)card->raw_scr, 8);

		mmc_wait_for_req(host, &mrq);

		if (cmd.error != MMC_ERR_NONE || data.error != MMC_ERR_NONE) {
			mmc_card_set_dead(card);
			continue;
		}

		card->raw_scr[0] = ntohl(card->raw_scr[0]);
		card->raw_scr[1] = ntohl(card->raw_scr[1]);

		mmc_decode_scr(card);
	}

	mmc_deselect_cards(host);
}

static void mmc_read_switch_caps(struct mmc_host *host)
{
	int err;
	struct mmc_card *card;
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	unsigned char *status;
	struct scatterlist sg;

	if (!(host->caps & MMC_CAP_SD_HIGHSPEED))
		return;

	status = kmalloc(64, GFP_KERNEL);
	if (!status) {
		printk(KERN_WARNING "%s: Unable to allocate buffer for "
			"reading switch capabilities.\n",
			mmc_hostname(host));
		return;
	}

	list_for_each_entry(card, &host->cards, node) {
		if (card->state & (MMC_STATE_DEAD|MMC_STATE_PRESENT))
			continue;
		if (!mmc_card_sd(card))
			continue;
		if (card->scr.sda_vsn < SCR_SPEC_VER_1)
			continue;

		err = mmc_select_card(host, card);
		if (err != MMC_ERR_NONE) {
			mmc_card_set_dead(card);
			continue;
		}

		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = SD_SWITCH;
		cmd.arg = 0x00FFFFF1;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		memset(&data, 0, sizeof(struct mmc_data));

		mmc_set_data_timeout(&data, card, 0);

		data.blksz = 64;
		data.blocks = 1;
		data.flags = MMC_DATA_READ;
		data.sg = &sg;
		data.sg_len = 1;

		memset(&mrq, 0, sizeof(struct mmc_request));

		mrq.cmd = &cmd;
		mrq.data = &data;

		sg_init_one(&sg, status, 64);

		mmc_wait_for_req(host, &mrq);

		if (cmd.error != MMC_ERR_NONE || data.error != MMC_ERR_NONE) {
			printk("%s: unable to read switch capabilities, "
				"performance might suffer.\n",
				mmc_hostname(card->host));
			continue;
		}

		if (status[13] & 0x02)
			card->sw_caps.hs_max_dtr = 50000000;

		memset(&cmd, 0, sizeof(struct mmc_command));

		cmd.opcode = SD_SWITCH;
		cmd.arg = 0x80FFFFF1;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		memset(&data, 0, sizeof(struct mmc_data));

		mmc_set_data_timeout(&data, card, 0);

		data.blksz = 64;
		data.blocks = 1;
		data.flags = MMC_DATA_READ;
		data.sg = &sg;
		data.sg_len = 1;

		memset(&mrq, 0, sizeof(struct mmc_request));

		mrq.cmd = &cmd;
		mrq.data = &data;

		sg_init_one(&sg, status, 64);

		mmc_wait_for_req(host, &mrq);

		if (cmd.error != MMC_ERR_NONE || data.error != MMC_ERR_NONE ||
			(status[16] & 0xF) != 1) {
			printk(KERN_WARNING "%s: Problem switching card "
				"into high-speed mode!\n",
				mmc_hostname(host));
			continue;
		}

		mmc_card_set_highspeed(card);

		host->ios.timing = MMC_TIMING_SD_HS;
		mmc_set_ios(host);
	}

	kfree(status);

	mmc_deselect_cards(host);
}

static unsigned int mmc_calculate_clock(struct mmc_host *host)
{
	struct mmc_card *card;
	unsigned int max_dtr = host->f_max;

	list_for_each_entry(card, &host->cards, node)
		if (!mmc_card_dead(card)) {
			if (mmc_card_highspeed(card) && mmc_card_sd(card)) {
				if (max_dtr > card->sw_caps.hs_max_dtr)
					max_dtr = card->sw_caps.hs_max_dtr;
			} else if (mmc_card_highspeed(card) && !mmc_card_sd(card)) {
				if (max_dtr > card->ext_csd.hs_max_dtr)
					max_dtr = card->ext_csd.hs_max_dtr;
			} else if (max_dtr > card->csd.max_dtr) {
				max_dtr = card->csd.max_dtr;
			}
		}

	pr_debug("%s: selected %d.%03dMHz transfer rate\n",
		 mmc_hostname(host),
		 max_dtr / 1000000, (max_dtr / 1000) % 1000);

	return max_dtr;
}

/*
 * Check whether cards we already know about are still present.
 * We do this by requesting status, and checking whether a card
 * responds.
 *
 * A request for status does not cause a state change in data
 * transfer mode.
 */
static void mmc_check_cards(struct mmc_host *host)
{
	struct list_head *l, *n;

	mmc_deselect_cards(host);

	list_for_each_safe(l, n, &host->cards) {
		struct mmc_card *card = mmc_list_to_card(l);
		struct mmc_command cmd;
		int err;

		cmd.opcode = MMC_SEND_STATUS;
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

		err = mmc_wait_for_cmd(host, &cmd, CMD_RETRIES);
		if (err == MMC_ERR_NONE)
			continue;

		mmc_card_set_dead(card);
	}
}

static void mmc_setup(struct mmc_host *host)
{
	if (host->ios.power_mode != MMC_POWER_ON) {
		int err;
		u32 ocr;

		host->mode = MMC_MODE_SD;

		mmc_power_up(host);
		mmc_idle_cards(host);

		err = mmc_send_if_cond(host, host->ocr_avail, NULL);
		if (err != MMC_ERR_NONE) {
			return;
		}
		err = mmc_send_app_op_cond(host, 0, &ocr);

		/*
		 * If we fail to detect any SD cards then try
		 * searching for MMC cards.
		 */
		if (err != MMC_ERR_NONE) {
			host->mode = MMC_MODE_MMC;

			err = mmc_send_op_cond(host, 0, &ocr);
			if (err != MMC_ERR_NONE)
				return;
		}

		host->ocr = mmc_select_voltage(host, ocr);

		/*
		 * Since we're changing the OCR value, we seem to
		 * need to tell some cards to go back to the idle
		 * state.  We wait 1ms to give cards time to
		 * respond.
		 */
		if (host->ocr)
			mmc_idle_cards(host);
	} else {
		host->ios.bus_mode = MMC_BUSMODE_OPENDRAIN;
		host->ios.clock = host->f_min;
		mmc_set_ios(host);

		/*
		 * We should remember the OCR mask from the existing
		 * cards, and detect the new cards OCR mask, combine
		 * the two and re-select the VDD.  However, if we do
		 * change VDD, we should do an idle, and then do a
		 * full re-initialisation.  We would need to notify
		 * drivers so that they can re-setup the cards as
		 * well, while keeping their queues at bay.
		 *
		 * For the moment, we take the easy way out - if the
		 * new cards don't like our currently selected VDD,
		 * they drop off the bus.
		 */
	}

	if (host->ocr == 0)
		return;

	/*
	 * Send the selected OCR multiple times... until the cards
	 * all get the idea that they should be ready for CMD2.
	 * (My SanDisk card seems to need this.)
	 */
	if (host->mode == MMC_MODE_SD) {
		int err, sd2;
		err = mmc_send_if_cond(host, host->ocr, &sd2);
		if (err == MMC_ERR_NONE) {
			/*
			* If SD_SEND_IF_COND indicates an SD 2.0
			* compliant card and we should set bit 30
			* of the ocr to indicate that we can handle
			* block-addressed SDHC cards.
			*/
			mmc_send_app_op_cond(host, host->ocr | (sd2 << 30), NULL);
		}
	} else {
		mmc_send_op_cond(host, host->ocr, NULL);
	}

	mmc_discover_cards(host);

	/*
	 * Ok, now switch to push-pull mode.
	 */
	host->ios.bus_mode = MMC_BUSMODE_PUSHPULL;
	mmc_set_ios(host);

	mmc_read_csds(host);

	if (host->mode == MMC_MODE_SD) {
		mmc_read_scrs(host);
		mmc_read_switch_caps(host);
	} else
		mmc_process_ext_csds(host);
}


/**
 *	mmc_detect_change - process change of state on a MMC socket
 *	@host: host which changed state.
 *	@delay: optional delay to wait before detection (jiffies)
 *
 *	All we know is that card(s) have been inserted or removed
 *	from the socket(s).  We don't know which socket or cards.
 */
void mmc_detect_change(struct mmc_host *host, unsigned long delay)
{
	mmc_schedule_delayed_work(&host->detect, delay);
}

EXPORT_SYMBOL(mmc_detect_change);


static void mmc_rescan(struct work_struct *work)
{
	struct mmc_host *host =
		container_of(work, struct mmc_host, detect.work);
	struct list_head *l, *n;
	unsigned char power_mode;

	mmc_claim_host(host);

	/*
	 * Check for removed cards and newly inserted ones. We check for
	 * removed cards first so we can intelligently re-select the VDD.
	 */
	power_mode = host->ios.power_mode;
	if (power_mode == MMC_POWER_ON)
		mmc_check_cards(host);

	mmc_setup(host);

	/*
	 * Some broken cards process CMD1 even in stand-by state. There is
	 * no reply, but an ILLEGAL_COMMAND error is cached and returned
	 * after next command. We poll for card status here to clear any
	 * possibly pending error.
	 */
	if (power_mode == MMC_POWER_ON)
		mmc_check_cards(host);

	if (!list_empty(&host->cards)) {
		/*
		 * (Re-)calculate the fastest clock rate which the
		 * attached cards and the host support.
		 */
		host->ios.clock = mmc_calculate_clock(host);
		mmc_set_ios(host);
	}

	mmc_release_host(host);

	list_for_each_safe(l, n, &host->cards) {
		struct mmc_card *card = mmc_list_to_card(l);

		/*
		 * If this is a new and good card, register it.
		 */
		if (!mmc_card_present(card) && !mmc_card_dead(card)) {
			if (mmc_register_card(card))
				mmc_card_set_dead(card);
			else
				mmc_card_set_present(card);
		}

		/*
		 * If this card is dead, destroy it.
		 */
		if (mmc_card_dead(card)) {
			list_del(&card->node);
			mmc_remove_card(card);
		}
	}

	/*
	 * If we discover that there are no cards on the
	 * bus, turn off the clock and power down.
	 */
	if (list_empty(&host->cards))
		mmc_power_off(host);
}


/**
 *	mmc_alloc_host - initialise the per-host structure.
 *	@extra: sizeof private data structure
 *	@dev: pointer to host device model structure
 *
 *	Initialise the per-host structure.
 */
struct mmc_host *mmc_alloc_host(int extra, struct device *dev)
{
	struct mmc_host *host;

	host = mmc_alloc_host_sysfs(extra, dev);
	if (host) {
		spin_lock_init(&host->lock);
		init_waitqueue_head(&host->wq);
		INIT_LIST_HEAD(&host->cards);
		INIT_DELAYED_WORK(&host->detect, mmc_rescan);

		/*
		 * By default, hosts do not support SGIO or large requests.
		 * They have to set these according to their abilities.
		 */
		host->max_hw_segs = 1;
		host->max_phys_segs = 1;
		host->max_seg_size = PAGE_CACHE_SIZE;

		host->max_req_size = PAGE_CACHE_SIZE;
		host->max_blk_size = 512;
		host->max_blk_count = PAGE_CACHE_SIZE / 512;
	}

	return host;
}

EXPORT_SYMBOL(mmc_alloc_host);

/**
 *	mmc_add_host - initialise host hardware
 *	@host: mmc host
 */
int mmc_add_host(struct mmc_host *host)
{
	int ret;

	ret = mmc_add_host_sysfs(host);
	if (ret == 0) {
		mmc_power_off(host);
		mmc_detect_change(host, 0);
	}

	return ret;
}

EXPORT_SYMBOL(mmc_add_host);

/**
 *	mmc_remove_host - remove host hardware
 *	@host: mmc host
 *
 *	Unregister and remove all cards associated with this host,
 *	and power down the MMC bus.
 */
void mmc_remove_host(struct mmc_host *host)
{
	struct list_head *l, *n;

	list_for_each_safe(l, n, &host->cards) {
		struct mmc_card *card = mmc_list_to_card(l);

		mmc_remove_card(card);
	}

	mmc_power_off(host);
	mmc_remove_host_sysfs(host);
}

EXPORT_SYMBOL(mmc_remove_host);

/**
 *	mmc_free_host - free the host structure
 *	@host: mmc host
 *
 *	Free the host once all references to it have been dropped.
 */
void mmc_free_host(struct mmc_host *host)
{
	mmc_flush_scheduled_work();
	mmc_free_host_sysfs(host);
}

EXPORT_SYMBOL(mmc_free_host);

#ifdef CONFIG_PM

/**
 *	mmc_suspend_host - suspend a host
 *	@host: mmc host
 *	@state: suspend mode (PM_SUSPEND_xxx)
 */
int mmc_suspend_host(struct mmc_host *host, pm_message_t state)
{
	mmc_claim_host(host);
	mmc_deselect_cards(host);
	mmc_power_off(host);
	mmc_release_host(host);

	return 0;
}

EXPORT_SYMBOL(mmc_suspend_host);

/**
 *	mmc_resume_host - resume a previously suspended host
 *	@host: mmc host
 */
int mmc_resume_host(struct mmc_host *host)
{
	mmc_rescan(&host->detect.work);

	return 0;
}

EXPORT_SYMBOL(mmc_resume_host);

#endif

MODULE_LICENSE("GPL");
