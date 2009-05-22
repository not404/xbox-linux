/*
 *  Copyright (C) 2006 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation, version 2 of the
 *  License.
 *
 *  Jun 2006 - namespaces support
 *             OpenVZ, SWsoft Inc.
 *             Pavel Emelianov <xemul@openvz.org>
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/nsproxy.h>
#include <linux/init_task.h>
#include <linux/namespace.h>
#include <linux/utsname.h>

struct nsproxy init_nsproxy = INIT_NSPROXY(init_nsproxy);

static inline void get_nsproxy(struct nsproxy *ns)
{
	atomic_inc(&ns->count);
}

void get_task_namespaces(struct task_struct *tsk)
{
	struct nsproxy *ns = tsk->nsproxy;
	if (ns) {
		get_nsproxy(ns);
	}
}

/*
 * creates a copy of "orig" with refcount 1.
 * This does not grab references to the contained namespaces,
 * so that needs to be done by dup_namespaces.
 */
static inline struct nsproxy *clone_namespaces(struct nsproxy *orig)
{
	struct nsproxy *ns;

	ns = kmemdup(orig, sizeof(struct nsproxy), GFP_KERNEL);
	if (ns)
		atomic_set(&ns->count, 1);
	return ns;
}

/*
 * copies the nsproxy, setting refcount to 1, and grabbing a
 * reference to all contained namespaces.  Called from
 * sys_unshare()
 */
struct nsproxy *dup_namespaces(struct nsproxy *orig)
{
	struct nsproxy *ns = clone_namespaces(orig);

	if (ns) {
		if (ns->namespace)
			get_namespace(ns->namespace);
		if (ns->uts_ns)
			get_uts_ns(ns->uts_ns);
		if (ns->ipc_ns)
			get_ipc_ns(ns->ipc_ns);
	}

	return ns;
}

/*
 * called from clone.  This now handles copy for nsproxy and all
 * namespaces therein.
 */
int copy_namespaces(int flags, struct task_struct *tsk)
{
	struct nsproxy *old_ns = tsk->nsproxy;
	struct nsproxy *new_ns;
	int err = 0;

	if (!old_ns)
		return 0;

	get_nsproxy(old_ns);

	if (!(flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC)))
		return 0;

	new_ns = clone_namespaces(old_ns);
	if (!new_ns) {
		err = -ENOMEM;
		goto out;
	}

	tsk->nsproxy = new_ns;

	err = copy_namespace(flags, tsk);
	if (err)
		goto out_ns;

	err = copy_utsname(flags, tsk);
	if (err)
		goto out_uts;

	err = copy_ipcs(flags, tsk);
	if (err)
		goto out_ipc;

out:
	put_nsproxy(old_ns);
	return err;

out_ipc:
	if (new_ns->uts_ns)
		put_uts_ns(new_ns->uts_ns);
out_uts:
	if (new_ns->namespace)
		put_namespace(new_ns->namespace);
out_ns:
	tsk->nsproxy = old_ns;
	kfree(new_ns);
	goto out;
}

void free_nsproxy(struct nsproxy *ns)
{
		if (ns->namespace)
			put_namespace(ns->namespace);
		if (ns->uts_ns)
			put_uts_ns(ns->uts_ns);
		if (ns->ipc_ns)
			put_ipc_ns(ns->ipc_ns);
		kfree(ns);
}
