/*
 *  arch/s390/kernel/ipl.c
 *    ipl/reipl/dump support for Linux on s390.
 *
 *    Copyright (C) IBM Corp. 2005,2006
 *    Author(s): Michael Holzheu <holzheu@de.ibm.com>
 *		 Heiko Carstens <heiko.carstens@de.ibm.com>
 *		 Volker Sameske <sameske@de.ibm.com>
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <asm/smp.h>
#include <asm/setup.h>
#include <asm/cpcmd.h>
#include <asm/cio.h>

#define IPL_PARM_BLOCK_VERSION 0

enum ipl_type {
	IPL_TYPE_NONE	 = 1,
	IPL_TYPE_UNKNOWN = 2,
	IPL_TYPE_CCW	 = 4,
	IPL_TYPE_FCP	 = 8,
};

#define IPL_NONE_STR	 "none"
#define IPL_UNKNOWN_STR  "unknown"
#define IPL_CCW_STR	 "ccw"
#define IPL_FCP_STR	 "fcp"

static char *ipl_type_str(enum ipl_type type)
{
	switch (type) {
	case IPL_TYPE_NONE:
		return IPL_NONE_STR;
	case IPL_TYPE_CCW:
		return IPL_CCW_STR;
	case IPL_TYPE_FCP:
		return IPL_FCP_STR;
	case IPL_TYPE_UNKNOWN:
	default:
		return IPL_UNKNOWN_STR;
	}
}

enum ipl_method {
	IPL_METHOD_NONE,
	IPL_METHOD_CCW_CIO,
	IPL_METHOD_CCW_DIAG,
	IPL_METHOD_CCW_VM,
	IPL_METHOD_FCP_RO_DIAG,
	IPL_METHOD_FCP_RW_DIAG,
	IPL_METHOD_FCP_RO_VM,
};

enum shutdown_action {
	SHUTDOWN_REIPL,
	SHUTDOWN_DUMP,
	SHUTDOWN_STOP,
};

#define SHUTDOWN_REIPL_STR "reipl"
#define SHUTDOWN_DUMP_STR  "dump"
#define SHUTDOWN_STOP_STR  "stop"

static char *shutdown_action_str(enum shutdown_action action)
{
	switch (action) {
	case SHUTDOWN_REIPL:
		return SHUTDOWN_REIPL_STR;
	case SHUTDOWN_DUMP:
		return SHUTDOWN_DUMP_STR;
	case SHUTDOWN_STOP:
		return SHUTDOWN_STOP_STR;
	default:
		BUG();
	}
}

enum diag308_subcode  {
	DIAG308_IPL   = 3,
	DIAG308_DUMP  = 4,
	DIAG308_SET   = 5,
	DIAG308_STORE = 6,
};

enum diag308_ipl_type {
	DIAG308_IPL_TYPE_FCP = 0,
	DIAG308_IPL_TYPE_CCW = 2,
};

enum diag308_opt {
	DIAG308_IPL_OPT_IPL  = 0x10,
	DIAG308_IPL_OPT_DUMP = 0x20,
};

enum diag308_rc {
	DIAG308_RC_OK = 1,
};

static int diag308_set_works = 0;

static int reipl_capabilities = IPL_TYPE_UNKNOWN;
static enum ipl_type reipl_type = IPL_TYPE_UNKNOWN;
static enum ipl_method reipl_method = IPL_METHOD_NONE;
static struct ipl_parameter_block *reipl_block_fcp;
static struct ipl_parameter_block *reipl_block_ccw;

static int dump_capabilities = IPL_TYPE_NONE;
static enum ipl_type dump_type = IPL_TYPE_NONE;
static enum ipl_method dump_method = IPL_METHOD_NONE;
static struct ipl_parameter_block *dump_block_fcp;
static struct ipl_parameter_block *dump_block_ccw;

static enum shutdown_action on_panic_action = SHUTDOWN_STOP;

static int diag308(unsigned long subcode, void *addr)
{
	register unsigned long _addr asm("0") = (unsigned long) addr;
	register unsigned long _rc asm("1") = 0;

	asm volatile(
		"	diag	%0,%2,0x308\n"
		"0:\n"
		EX_TABLE(0b,0b)
		: "+d" (_addr), "+d" (_rc)
		: "d" (subcode) : "cc", "memory");
	return _rc;
}

/* SYSFS */

#define DEFINE_IPL_ATTR_RO(_prefix, _name, _format, _value)		\
static ssize_t sys_##_prefix##_##_name##_show(struct subsystem *subsys,	\
		char *page)						\
{									\
	return sprintf(page, _format, _value);				\
}									\
static struct subsys_attribute sys_##_prefix##_##_name##_attr =		\
	__ATTR(_name, S_IRUGO, sys_##_prefix##_##_name##_show, NULL);

#define DEFINE_IPL_ATTR_RW(_prefix, _name, _fmt_out, _fmt_in, _value)	\
static ssize_t sys_##_prefix##_##_name##_show(struct subsystem *subsys,	\
		char *page)						\
{									\
	return sprintf(page, _fmt_out,					\
			(unsigned long long) _value);			\
}									\
static ssize_t sys_##_prefix##_##_name##_store(struct subsystem *subsys,\
		const char *buf, size_t len)				\
{									\
	unsigned long long value;					\
	if (sscanf(buf, _fmt_in, &value) != 1)				\
		return -EINVAL;						\
	_value = value;							\
	return len;							\
}									\
static struct subsys_attribute sys_##_prefix##_##_name##_attr =		\
	__ATTR(_name,(S_IRUGO | S_IWUSR),				\
			sys_##_prefix##_##_name##_show,			\
			sys_##_prefix##_##_name##_store);

static void make_attrs_ro(struct attribute **attrs)
{
	while (*attrs) {
		(*attrs)->mode = S_IRUGO;
		attrs++;
	}
}

/*
 * ipl section
 */

static enum ipl_type ipl_get_type(void)
{
	struct ipl_parameter_block *ipl = IPL_PARMBLOCK_START;

	if (!(ipl_flags & IPL_DEVNO_VALID))
		return IPL_TYPE_UNKNOWN;
	if (!(ipl_flags & IPL_PARMBLOCK_VALID))
		return IPL_TYPE_CCW;
	if (ipl->hdr.version > IPL_MAX_SUPPORTED_VERSION)
		return IPL_TYPE_UNKNOWN;
	if (ipl->hdr.pbt != DIAG308_IPL_TYPE_FCP)
		return IPL_TYPE_UNKNOWN;
	return IPL_TYPE_FCP;
}

static ssize_t ipl_type_show(struct subsystem *subsys, char *page)
{
	return sprintf(page, "%s\n", ipl_type_str(ipl_get_type()));
}

static struct subsys_attribute sys_ipl_type_attr = __ATTR_RO(ipl_type);

static ssize_t sys_ipl_device_show(struct subsystem *subsys, char *page)
{
	struct ipl_parameter_block *ipl = IPL_PARMBLOCK_START;

	switch (ipl_get_type()) {
	case IPL_TYPE_CCW:
		return sprintf(page, "0.0.%04x\n", ipl_devno);
	case IPL_TYPE_FCP:
		return sprintf(page, "0.0.%04x\n", ipl->ipl_info.fcp.devno);
	default:
		return 0;
	}
}

static struct subsys_attribute sys_ipl_device_attr =
	__ATTR(device, S_IRUGO, sys_ipl_device_show, NULL);

static ssize_t ipl_parameter_read(struct kobject *kobj, char *buf, loff_t off,
				  size_t count)
{
	unsigned int size = IPL_PARMBLOCK_SIZE;

	if (off > size)
		return 0;
	if (off + count > size)
		count = size - off;
	memcpy(buf, (void *)IPL_PARMBLOCK_START + off, count);
	return count;
}

static struct bin_attribute ipl_parameter_attr = {
	.attr = {
		.name = "binary_parameter",
		.mode = S_IRUGO,
		.owner = THIS_MODULE,
	},
	.size = PAGE_SIZE,
	.read = &ipl_parameter_read,
};

static ssize_t ipl_scp_data_read(struct kobject *kobj, char *buf, loff_t off,
	size_t count)
{
	unsigned int size = IPL_PARMBLOCK_START->ipl_info.fcp.scp_data_len;
	void *scp_data = &IPL_PARMBLOCK_START->ipl_info.fcp.scp_data;

	if (off > size)
		return 0;
	if (off + count > size)
		count = size - off;
	memcpy(buf, scp_data + off, count);
	return count;
}

static struct bin_attribute ipl_scp_data_attr = {
	.attr = {
		.name = "scp_data",
		.mode = S_IRUGO,
		.owner = THIS_MODULE,
	},
	.size = PAGE_SIZE,
	.read = &ipl_scp_data_read,
};

/* FCP ipl device attributes */

DEFINE_IPL_ATTR_RO(ipl_fcp, wwpn, "0x%016llx\n", (unsigned long long)
		   IPL_PARMBLOCK_START->ipl_info.fcp.wwpn);
DEFINE_IPL_ATTR_RO(ipl_fcp, lun, "0x%016llx\n", (unsigned long long)
		   IPL_PARMBLOCK_START->ipl_info.fcp.lun);
DEFINE_IPL_ATTR_RO(ipl_fcp, bootprog, "%lld\n", (unsigned long long)
		   IPL_PARMBLOCK_START->ipl_info.fcp.bootprog);
DEFINE_IPL_ATTR_RO(ipl_fcp, br_lba, "%lld\n", (unsigned long long)
		   IPL_PARMBLOCK_START->ipl_info.fcp.br_lba);

static struct attribute *ipl_fcp_attrs[] = {
	&sys_ipl_type_attr.attr,
	&sys_ipl_device_attr.attr,
	&sys_ipl_fcp_wwpn_attr.attr,
	&sys_ipl_fcp_lun_attr.attr,
	&sys_ipl_fcp_bootprog_attr.attr,
	&sys_ipl_fcp_br_lba_attr.attr,
	NULL,
};

static struct attribute_group ipl_fcp_attr_group = {
	.attrs = ipl_fcp_attrs,
};

/* CCW ipl device attributes */

static struct attribute *ipl_ccw_attrs[] = {
	&sys_ipl_type_attr.attr,
	&sys_ipl_device_attr.attr,
	NULL,
};

static struct attribute_group ipl_ccw_attr_group = {
	.attrs = ipl_ccw_attrs,
};

/* UNKNOWN ipl device attributes */

static struct attribute *ipl_unknown_attrs[] = {
	&sys_ipl_type_attr.attr,
	NULL,
};

static struct attribute_group ipl_unknown_attr_group = {
	.attrs = ipl_unknown_attrs,
};

static decl_subsys(ipl, NULL, NULL);

/*
 * reipl section
 */

/* FCP reipl device attributes */

DEFINE_IPL_ATTR_RW(reipl_fcp, wwpn, "0x%016llx\n", "%016llx\n",
		   reipl_block_fcp->ipl_info.fcp.wwpn);
DEFINE_IPL_ATTR_RW(reipl_fcp, lun, "0x%016llx\n", "%016llx\n",
		   reipl_block_fcp->ipl_info.fcp.lun);
DEFINE_IPL_ATTR_RW(reipl_fcp, bootprog, "%lld\n", "%lld\n",
		   reipl_block_fcp->ipl_info.fcp.bootprog);
DEFINE_IPL_ATTR_RW(reipl_fcp, br_lba, "%lld\n", "%lld\n",
		   reipl_block_fcp->ipl_info.fcp.br_lba);
DEFINE_IPL_ATTR_RW(reipl_fcp, device, "0.0.%04llx\n", "0.0.%llx\n",
		   reipl_block_fcp->ipl_info.fcp.devno);

static struct attribute *reipl_fcp_attrs[] = {
	&sys_reipl_fcp_device_attr.attr,
	&sys_reipl_fcp_wwpn_attr.attr,
	&sys_reipl_fcp_lun_attr.attr,
	&sys_reipl_fcp_bootprog_attr.attr,
	&sys_reipl_fcp_br_lba_attr.attr,
	NULL,
};

static struct attribute_group reipl_fcp_attr_group = {
	.name  = IPL_FCP_STR,
	.attrs = reipl_fcp_attrs,
};

/* CCW reipl device attributes */

DEFINE_IPL_ATTR_RW(reipl_ccw, device, "0.0.%04llx\n", "0.0.%llx\n",
	reipl_block_ccw->ipl_info.ccw.devno);

static struct attribute *reipl_ccw_attrs[] = {
	&sys_reipl_ccw_device_attr.attr,
	NULL,
};

static struct attribute_group reipl_ccw_attr_group = {
	.name  = IPL_CCW_STR,
	.attrs = reipl_ccw_attrs,
};

/* reipl type */

static int reipl_set_type(enum ipl_type type)
{
	if (!(reipl_capabilities & type))
		return -EINVAL;

	switch(type) {
	case IPL_TYPE_CCW:
		if (MACHINE_IS_VM)
			reipl_method = IPL_METHOD_CCW_VM;
		else
			reipl_method = IPL_METHOD_CCW_CIO;
		break;
	case IPL_TYPE_FCP:
		if (diag308_set_works)
			reipl_method = IPL_METHOD_FCP_RW_DIAG;
		else if (MACHINE_IS_VM)
			reipl_method = IPL_METHOD_FCP_RO_VM;
		else
			reipl_method = IPL_METHOD_FCP_RO_DIAG;
		break;
	default:
		reipl_method = IPL_METHOD_NONE;
	}
	reipl_type = type;
	return 0;
}

static ssize_t reipl_type_show(struct subsystem *subsys, char *page)
{
	return sprintf(page, "%s\n", ipl_type_str(reipl_type));
}

static ssize_t reipl_type_store(struct subsystem *subsys, const char *buf,
				size_t len)
{
	int rc = -EINVAL;

	if (strncmp(buf, IPL_CCW_STR, strlen(IPL_CCW_STR)) == 0)
		rc = reipl_set_type(IPL_TYPE_CCW);
	else if (strncmp(buf, IPL_FCP_STR, strlen(IPL_FCP_STR)) == 0)
		rc = reipl_set_type(IPL_TYPE_FCP);
	return (rc != 0) ? rc : len;
}

static struct subsys_attribute reipl_type_attr =
		__ATTR(reipl_type, 0644, reipl_type_show, reipl_type_store);

static decl_subsys(reipl, NULL, NULL);

/*
 * dump section
 */

/* FCP dump device attributes */

DEFINE_IPL_ATTR_RW(dump_fcp, wwpn, "0x%016llx\n", "%016llx\n",
		   dump_block_fcp->ipl_info.fcp.wwpn);
DEFINE_IPL_ATTR_RW(dump_fcp, lun, "0x%016llx\n", "%016llx\n",
		   dump_block_fcp->ipl_info.fcp.lun);
DEFINE_IPL_ATTR_RW(dump_fcp, bootprog, "%lld\n", "%lld\n",
		   dump_block_fcp->ipl_info.fcp.bootprog);
DEFINE_IPL_ATTR_RW(dump_fcp, br_lba, "%lld\n", "%lld\n",
		   dump_block_fcp->ipl_info.fcp.br_lba);
DEFINE_IPL_ATTR_RW(dump_fcp, device, "0.0.%04llx\n", "0.0.%llx\n",
		   dump_block_fcp->ipl_info.fcp.devno);

static struct attribute *dump_fcp_attrs[] = {
	&sys_dump_fcp_device_attr.attr,
	&sys_dump_fcp_wwpn_attr.attr,
	&sys_dump_fcp_lun_attr.attr,
	&sys_dump_fcp_bootprog_attr.attr,
	&sys_dump_fcp_br_lba_attr.attr,
	NULL,
};

static struct attribute_group dump_fcp_attr_group = {
	.name  = IPL_FCP_STR,
	.attrs = dump_fcp_attrs,
};

/* CCW dump device attributes */

DEFINE_IPL_ATTR_RW(dump_ccw, device, "0.0.%04llx\n", "0.0.%llx\n",
		   dump_block_ccw->ipl_info.ccw.devno);

static struct attribute *dump_ccw_attrs[] = {
	&sys_dump_ccw_device_attr.attr,
	NULL,
};

static struct attribute_group dump_ccw_attr_group = {
	.name  = IPL_CCW_STR,
	.attrs = dump_ccw_attrs,
};

/* dump type */

static int dump_set_type(enum ipl_type type)
{
	if (!(dump_capabilities & type))
		return -EINVAL;
	switch(type) {
	case IPL_TYPE_CCW:
		if (MACHINE_IS_VM)
			dump_method = IPL_METHOD_CCW_VM;
		else
			dump_method = IPL_METHOD_CCW_CIO;
		break;
	case IPL_TYPE_FCP:
		dump_method = IPL_METHOD_FCP_RW_DIAG;
		break;
	default:
		dump_method = IPL_METHOD_NONE;
	}
	dump_type = type;
	return 0;
}

static ssize_t dump_type_show(struct subsystem *subsys, char *page)
{
	return sprintf(page, "%s\n", ipl_type_str(dump_type));
}

static ssize_t dump_type_store(struct subsystem *subsys, const char *buf,
			       size_t len)
{
	int rc = -EINVAL;

	if (strncmp(buf, IPL_NONE_STR, strlen(IPL_NONE_STR)) == 0)
		rc = dump_set_type(IPL_TYPE_NONE);
	else if (strncmp(buf, IPL_CCW_STR, strlen(IPL_CCW_STR)) == 0)
		rc = dump_set_type(IPL_TYPE_CCW);
	else if (strncmp(buf, IPL_FCP_STR, strlen(IPL_FCP_STR)) == 0)
		rc = dump_set_type(IPL_TYPE_FCP);
	return (rc != 0) ? rc : len;
}

static struct subsys_attribute dump_type_attr =
		__ATTR(dump_type, 0644, dump_type_show, dump_type_store);

static decl_subsys(dump, NULL, NULL);

#ifdef CONFIG_SMP
static void dump_smp_stop_all(void)
{
	int cpu;
	preempt_disable();
	for_each_online_cpu(cpu) {
		if (cpu == smp_processor_id())
			continue;
		while (signal_processor(cpu, sigp_stop) == sigp_busy)
			udelay(10);
	}
	preempt_enable();
}
#else
#define dump_smp_stop_all() do { } while (0)
#endif

/*
 * Shutdown actions section
 */

static decl_subsys(shutdown_actions, NULL, NULL);

/* on panic */

static ssize_t on_panic_show(struct subsystem *subsys, char *page)
{
	return sprintf(page, "%s\n", shutdown_action_str(on_panic_action));
}

static ssize_t on_panic_store(struct subsystem *subsys, const char *buf,
			      size_t len)
{
	if (strncmp(buf, SHUTDOWN_REIPL_STR, strlen(SHUTDOWN_REIPL_STR)) == 0)
		on_panic_action = SHUTDOWN_REIPL;
	else if (strncmp(buf, SHUTDOWN_DUMP_STR,
			 strlen(SHUTDOWN_DUMP_STR)) == 0)
		on_panic_action = SHUTDOWN_DUMP;
	else if (strncmp(buf, SHUTDOWN_STOP_STR,
			 strlen(SHUTDOWN_STOP_STR)) == 0)
		on_panic_action = SHUTDOWN_STOP;
	else
		return -EINVAL;

	return len;
}

static struct subsys_attribute on_panic_attr =
		__ATTR(on_panic, 0644, on_panic_show, on_panic_store);

static void print_fcp_block(struct ipl_parameter_block *fcp_block)
{
	printk(KERN_EMERG "wwpn:      %016llx\n",
		(unsigned long long)fcp_block->ipl_info.fcp.wwpn);
	printk(KERN_EMERG "lun:       %016llx\n",
		(unsigned long long)fcp_block->ipl_info.fcp.lun);
	printk(KERN_EMERG "bootprog:  %lld\n",
		(unsigned long long)fcp_block->ipl_info.fcp.bootprog);
	printk(KERN_EMERG "br_lba:    %lld\n",
		(unsigned long long)fcp_block->ipl_info.fcp.br_lba);
	printk(KERN_EMERG "device:    %llx\n",
		(unsigned long long)fcp_block->ipl_info.fcp.devno);
	printk(KERN_EMERG "opt:       %x\n", fcp_block->ipl_info.fcp.opt);
}

void do_reipl(void)
{
	struct ccw_dev_id devid;
	static char buf[100];

	switch (reipl_type) {
	case IPL_TYPE_CCW:
		printk(KERN_EMERG "reboot on ccw device: 0.0.%04x\n",
			reipl_block_ccw->ipl_info.ccw.devno);
		break;
	case IPL_TYPE_FCP:
		printk(KERN_EMERG "reboot on fcp device:\n");
		print_fcp_block(reipl_block_fcp);
		break;
	default:
		break;
	}

	switch (reipl_method) {
	case IPL_METHOD_CCW_CIO:
		devid.devno = reipl_block_ccw->ipl_info.ccw.devno;
		devid.ssid  = 0;
		reipl_ccw_dev(&devid);
		break;
	case IPL_METHOD_CCW_VM:
		sprintf(buf, "IPL %X", reipl_block_ccw->ipl_info.ccw.devno);
		cpcmd(buf, NULL, 0, NULL);
		break;
	case IPL_METHOD_CCW_DIAG:
		diag308(DIAG308_SET, reipl_block_ccw);
		diag308(DIAG308_IPL, NULL);
		break;
	case IPL_METHOD_FCP_RW_DIAG:
		diag308(DIAG308_SET, reipl_block_fcp);
		diag308(DIAG308_IPL, NULL);
		break;
	case IPL_METHOD_FCP_RO_DIAG:
		diag308(DIAG308_IPL, NULL);
		break;
	case IPL_METHOD_FCP_RO_VM:
		cpcmd("IPL", NULL, 0, NULL);
		break;
	case IPL_METHOD_NONE:
	default:
		if (MACHINE_IS_VM)
			cpcmd("IPL", NULL, 0, NULL);
		diag308(DIAG308_IPL, NULL);
		break;
	}
	panic("reipl failed!\n");
}

static void do_dump(void)
{
	struct ccw_dev_id devid;
	static char buf[100];

	switch (dump_type) {
	case IPL_TYPE_CCW:
		printk(KERN_EMERG "Automatic dump on ccw device: 0.0.%04x\n",
		       dump_block_ccw->ipl_info.ccw.devno);
		break;
	case IPL_TYPE_FCP:
		printk(KERN_EMERG "Automatic dump on fcp device:\n");
		print_fcp_block(dump_block_fcp);
		break;
	default:
		return;
	}

	switch (dump_method) {
	case IPL_METHOD_CCW_CIO:
		dump_smp_stop_all();
		devid.devno = dump_block_ccw->ipl_info.ccw.devno;
		devid.ssid  = 0;
		reipl_ccw_dev(&devid);
		break;
	case IPL_METHOD_CCW_VM:
		dump_smp_stop_all();
		sprintf(buf, "STORE STATUS");
		cpcmd(buf, NULL, 0, NULL);
		sprintf(buf, "IPL %X", dump_block_ccw->ipl_info.ccw.devno);
		cpcmd(buf, NULL, 0, NULL);
		break;
	case IPL_METHOD_CCW_DIAG:
		diag308(DIAG308_SET, dump_block_ccw);
		diag308(DIAG308_DUMP, NULL);
		break;
	case IPL_METHOD_FCP_RW_DIAG:
		diag308(DIAG308_SET, dump_block_fcp);
		diag308(DIAG308_DUMP, NULL);
		break;
	case IPL_METHOD_NONE:
	default:
		return;
	}
	printk(KERN_EMERG "Dump failed!\n");
}

/* init functions */

static int __init ipl_register_fcp_files(void)
{
	int rc;

	rc = sysfs_create_group(&ipl_subsys.kset.kobj,
				&ipl_fcp_attr_group);
	if (rc)
		goto out;
	rc = sysfs_create_bin_file(&ipl_subsys.kset.kobj,
				   &ipl_parameter_attr);
	if (rc)
		goto out_ipl_parm;
	rc = sysfs_create_bin_file(&ipl_subsys.kset.kobj,
				   &ipl_scp_data_attr);
	if (!rc)
		goto out;

	sysfs_remove_bin_file(&ipl_subsys.kset.kobj, &ipl_parameter_attr);

out_ipl_parm:
	sysfs_remove_group(&ipl_subsys.kset.kobj, &ipl_fcp_attr_group);
out:
	return rc;
}

static int __init ipl_init(void)
{
	int rc;

	rc = firmware_register(&ipl_subsys);
	if (rc)
		return rc;
	switch (ipl_get_type()) {
	case IPL_TYPE_CCW:
		rc = sysfs_create_group(&ipl_subsys.kset.kobj,
					&ipl_ccw_attr_group);
		break;
	case IPL_TYPE_FCP:
		rc = ipl_register_fcp_files();
		break;
	default:
		rc = sysfs_create_group(&ipl_subsys.kset.kobj,
					&ipl_unknown_attr_group);
		break;
	}
	if (rc)
		firmware_unregister(&ipl_subsys);
	return rc;
}

static void __init reipl_probe(void)
{
	void *buffer;

	buffer = (void *) get_zeroed_page(GFP_KERNEL);
	if (!buffer)
		return;
	if (diag308(DIAG308_STORE, buffer) == DIAG308_RC_OK)
		diag308_set_works = 1;
	free_page((unsigned long)buffer);
}

static int __init reipl_ccw_init(void)
{
	int rc;

	reipl_block_ccw = (void *) get_zeroed_page(GFP_KERNEL);
	if (!reipl_block_ccw)
		return -ENOMEM;
	rc = sysfs_create_group(&reipl_subsys.kset.kobj, &reipl_ccw_attr_group);
	if (rc) {
		free_page((unsigned long)reipl_block_ccw);
		return rc;
	}
	reipl_block_ccw->hdr.len = IPL_PARM_BLK_CCW_LEN;
	reipl_block_ccw->hdr.version = IPL_PARM_BLOCK_VERSION;
	reipl_block_ccw->hdr.blk0_len = sizeof(reipl_block_ccw->ipl_info.ccw);
	reipl_block_ccw->hdr.pbt = DIAG308_IPL_TYPE_CCW;
	if (ipl_get_type() == IPL_TYPE_CCW)
		reipl_block_ccw->ipl_info.ccw.devno = ipl_devno;
	reipl_capabilities |= IPL_TYPE_CCW;
	return 0;
}

static int __init reipl_fcp_init(void)
{
	int rc;

	if ((!diag308_set_works) && (ipl_get_type() != IPL_TYPE_FCP))
		return 0;
	if ((!diag308_set_works) && (ipl_get_type() == IPL_TYPE_FCP))
		make_attrs_ro(reipl_fcp_attrs);

	reipl_block_fcp = (void *) get_zeroed_page(GFP_KERNEL);
	if (!reipl_block_fcp)
		return -ENOMEM;
	rc = sysfs_create_group(&reipl_subsys.kset.kobj, &reipl_fcp_attr_group);
	if (rc) {
		free_page((unsigned long)reipl_block_fcp);
		return rc;
	}
	if (ipl_get_type() == IPL_TYPE_FCP) {
		memcpy(reipl_block_fcp, IPL_PARMBLOCK_START, PAGE_SIZE);
	} else {
		reipl_block_fcp->hdr.len = IPL_PARM_BLK_FCP_LEN;
		reipl_block_fcp->hdr.version = IPL_PARM_BLOCK_VERSION;
		reipl_block_fcp->hdr.blk0_len =
			sizeof(reipl_block_fcp->ipl_info.fcp);
		reipl_block_fcp->hdr.pbt = DIAG308_IPL_TYPE_FCP;
		reipl_block_fcp->ipl_info.fcp.opt = DIAG308_IPL_OPT_IPL;
	}
	reipl_capabilities |= IPL_TYPE_FCP;
	return 0;
}

static int __init reipl_init(void)
{
	int rc;

	rc = firmware_register(&reipl_subsys);
	if (rc)
		return rc;
	rc = subsys_create_file(&reipl_subsys, &reipl_type_attr);
	if (rc) {
		firmware_unregister(&reipl_subsys);
		return rc;
	}
	rc = reipl_ccw_init();
	if (rc)
		return rc;
	rc = reipl_fcp_init();
	if (rc)
		return rc;
	rc = reipl_set_type(ipl_get_type());
	if (rc)
		return rc;
	return 0;
}

static int __init dump_ccw_init(void)
{
	int rc;

	dump_block_ccw = (void *) get_zeroed_page(GFP_KERNEL);
	if (!dump_block_ccw)
		return -ENOMEM;
	rc = sysfs_create_group(&dump_subsys.kset.kobj, &dump_ccw_attr_group);
	if (rc) {
		free_page((unsigned long)dump_block_ccw);
		return rc;
	}
	dump_block_ccw->hdr.len = IPL_PARM_BLK_CCW_LEN;
	dump_block_ccw->hdr.version = IPL_PARM_BLOCK_VERSION;
	dump_block_ccw->hdr.blk0_len = sizeof(reipl_block_ccw->ipl_info.ccw);
	dump_block_ccw->hdr.pbt = DIAG308_IPL_TYPE_CCW;
	dump_capabilities |= IPL_TYPE_CCW;
	return 0;
}

extern char s390_readinfo_sccb[];

static int __init dump_fcp_init(void)
{
	int rc;

	if(!(s390_readinfo_sccb[91] & 0x2))
		return 0; /* LDIPL DUMP is not installed */
	if (!diag308_set_works)
		return 0;
	dump_block_fcp = (void *) get_zeroed_page(GFP_KERNEL);
	if (!dump_block_fcp)
		return -ENOMEM;
	rc = sysfs_create_group(&dump_subsys.kset.kobj, &dump_fcp_attr_group);
	if (rc) {
		free_page((unsigned long)dump_block_fcp);
		return rc;
	}
	dump_block_fcp->hdr.len = IPL_PARM_BLK_FCP_LEN;
	dump_block_fcp->hdr.version = IPL_PARM_BLOCK_VERSION;
	dump_block_fcp->hdr.blk0_len = sizeof(dump_block_fcp->ipl_info.fcp);
	dump_block_fcp->hdr.pbt = DIAG308_IPL_TYPE_FCP;
	dump_block_fcp->ipl_info.fcp.opt = DIAG308_IPL_OPT_DUMP;
	dump_capabilities |= IPL_TYPE_FCP;
	return 0;
}

#define SHUTDOWN_ON_PANIC_PRIO 0

static int shutdown_on_panic_notify(struct notifier_block *self,
				    unsigned long event, void *data)
{
	if (on_panic_action == SHUTDOWN_DUMP)
		do_dump();
	else if (on_panic_action == SHUTDOWN_REIPL)
		do_reipl();
	return NOTIFY_OK;
}

static struct notifier_block shutdown_on_panic_nb = {
	.notifier_call = shutdown_on_panic_notify,
	.priority = SHUTDOWN_ON_PANIC_PRIO
};

static int __init dump_init(void)
{
	int rc;

	rc = firmware_register(&dump_subsys);
	if (rc)
		return rc;
	rc = subsys_create_file(&dump_subsys, &dump_type_attr);
	if (rc) {
		firmware_unregister(&dump_subsys);
		return rc;
	}
	rc = dump_ccw_init();
	if (rc)
		return rc;
	rc = dump_fcp_init();
	if (rc)
		return rc;
	dump_set_type(IPL_TYPE_NONE);
	return 0;
}

static int __init shutdown_actions_init(void)
{
	int rc;

	rc = firmware_register(&shutdown_actions_subsys);
	if (rc)
		return rc;
	rc = subsys_create_file(&shutdown_actions_subsys, &on_panic_attr);
	if (rc) {
		firmware_unregister(&shutdown_actions_subsys);
		return rc;
	}
	atomic_notifier_chain_register(&panic_notifier_list,
				       &shutdown_on_panic_nb);
	return 0;
}

static int __init s390_ipl_init(void)
{
	int rc;

	reipl_probe();
	rc = ipl_init();
	if (rc)
		return rc;
	rc = reipl_init();
	if (rc)
		return rc;
	rc = dump_init();
	if (rc)
		return rc;
	rc = shutdown_actions_init();
	if (rc)
		return rc;
	return 0;
}

__initcall(s390_ipl_init);
