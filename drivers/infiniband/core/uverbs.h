/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id: uverbs.h 2559 2005-06-06 19:43:16Z roland $
 */

#ifndef UVERBS_H
#define UVERBS_H

/* Include device.h and fs.h until cdev.h is self-sufficient */
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/kref.h>
#include <linux/idr.h>

#include <ib_verbs.h>
#include <ib_user_verbs.h>

struct ib_uverbs_device {
	int					devnum;
	struct cdev				dev;
	struct class_device			class_dev;
	struct ib_device		       *ib_dev;
	int					num_comp;
};

struct ib_uverbs_event_file {
	struct kref				ref;
	struct ib_uverbs_file		       *uverbs_file;
	spinlock_t				lock;
	int					fd;
	int					is_async;
	wait_queue_head_t			poll_wait;
	struct fasync_struct		       *async_queue;
	struct list_head			event_list;
};

struct ib_uverbs_file {
	struct kref				ref;
	struct ib_uverbs_device		       *device;
	struct ib_ucontext		       *ucontext;
	struct ib_event_handler			event_handler;
	struct ib_uverbs_event_file	        async_file;
	struct ib_uverbs_event_file	        comp_file[1];
};

struct ib_uverbs_async_event {
	struct ib_uverbs_async_event_desc	desc;
	struct list_head			list;
};

struct ib_uverbs_comp_event {
	struct ib_uverbs_comp_event_desc	desc;
	struct list_head			list;
};

struct ib_uobject_mr {
	struct ib_uobject			uobj;
	struct page			       *page_list;
	struct scatterlist		       *sg_list;
};

extern struct semaphore ib_uverbs_idr_mutex;
extern struct idr ib_uverbs_pd_idr;
extern struct idr ib_uverbs_mr_idr;
extern struct idr ib_uverbs_mw_idr;
extern struct idr ib_uverbs_ah_idr;
extern struct idr ib_uverbs_cq_idr;
extern struct idr ib_uverbs_qp_idr;

void ib_uverbs_comp_handler(struct ib_cq *cq, void *cq_context);
void ib_uverbs_cq_event_handler(struct ib_event *event, void *context_ptr);
void ib_uverbs_qp_event_handler(struct ib_event *event, void *context_ptr);

int ib_umem_get(struct ib_device *dev, struct ib_umem *mem,
		void *addr, size_t size, int write);
void ib_umem_release(struct ib_device *dev, struct ib_umem *umem);
void ib_umem_release_on_close(struct ib_device *dev, struct ib_umem *umem);

#define IB_UVERBS_DECLARE_CMD(name)					\
	ssize_t ib_uverbs_##name(struct ib_uverbs_file *file,		\
				 const char __user *buf, int in_len,	\
				 int out_len)

IB_UVERBS_DECLARE_CMD(query_params);
IB_UVERBS_DECLARE_CMD(get_context);
IB_UVERBS_DECLARE_CMD(query_device);
IB_UVERBS_DECLARE_CMD(query_port);
IB_UVERBS_DECLARE_CMD(query_gid);
IB_UVERBS_DECLARE_CMD(query_pkey);
IB_UVERBS_DECLARE_CMD(alloc_pd);
IB_UVERBS_DECLARE_CMD(dealloc_pd);
IB_UVERBS_DECLARE_CMD(reg_mr);
IB_UVERBS_DECLARE_CMD(dereg_mr);
IB_UVERBS_DECLARE_CMD(create_cq);
IB_UVERBS_DECLARE_CMD(destroy_cq);
IB_UVERBS_DECLARE_CMD(create_qp);
IB_UVERBS_DECLARE_CMD(modify_qp);
IB_UVERBS_DECLARE_CMD(destroy_qp);
IB_UVERBS_DECLARE_CMD(attach_mcast);
IB_UVERBS_DECLARE_CMD(detach_mcast);

#endif /* UVERBS_H */
