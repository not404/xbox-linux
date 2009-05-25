/*
 *  sata_vsc.c - Vitesse VSC7174 4 port DPA SATA
 *
 *  Maintained by:  Jeremy Higdon @ SGI
 * 		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2004 SGI
 *
 *  Bits from Jeff Garzik, Copyright RedHat, Inc.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 *  libata documentation is available via 'make {ps|pdf}docs',
 *  as Documentation/DocBook/libata.*
 *
 *  Vitesse hardware documentation presumably available under NDA.
 *  Intel 31244 (same hardware interface) documentation presumably
 *  available from http://developer.intel.com/
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>

#define DRV_NAME	"sata_vsc"
#define DRV_VERSION	"2.0"

enum {
	/* Interrupt register offsets (from chip base address) */
	VSC_SATA_INT_STAT_OFFSET	= 0x00,
	VSC_SATA_INT_MASK_OFFSET	= 0x04,

	/* Taskfile registers offsets */
	VSC_SATA_TF_CMD_OFFSET		= 0x00,
	VSC_SATA_TF_DATA_OFFSET		= 0x00,
	VSC_SATA_TF_ERROR_OFFSET	= 0x04,
	VSC_SATA_TF_FEATURE_OFFSET	= 0x06,
	VSC_SATA_TF_NSECT_OFFSET	= 0x08,
	VSC_SATA_TF_LBAL_OFFSET		= 0x0c,
	VSC_SATA_TF_LBAM_OFFSET		= 0x10,
	VSC_SATA_TF_LBAH_OFFSET		= 0x14,
	VSC_SATA_TF_DEVICE_OFFSET	= 0x18,
	VSC_SATA_TF_STATUS_OFFSET	= 0x1c,
	VSC_SATA_TF_COMMAND_OFFSET	= 0x1d,
	VSC_SATA_TF_ALTSTATUS_OFFSET	= 0x28,
	VSC_SATA_TF_CTL_OFFSET		= 0x29,

	/* DMA base */
	VSC_SATA_UP_DESCRIPTOR_OFFSET	= 0x64,
	VSC_SATA_UP_DATA_BUFFER_OFFSET	= 0x6C,
	VSC_SATA_DMA_CMD_OFFSET		= 0x70,

	/* SCRs base */
	VSC_SATA_SCR_STATUS_OFFSET	= 0x100,
	VSC_SATA_SCR_ERROR_OFFSET	= 0x104,
	VSC_SATA_SCR_CONTROL_OFFSET	= 0x108,

	/* Port stride */
	VSC_SATA_PORT_OFFSET		= 0x200,

	/* Error interrupt status bit offsets */
	VSC_SATA_INT_ERROR_CRC		= 0x40,
	VSC_SATA_INT_ERROR_T		= 0x20,
	VSC_SATA_INT_ERROR_P		= 0x10,
	VSC_SATA_INT_ERROR_R		= 0x8,
	VSC_SATA_INT_ERROR_E		= 0x4,
	VSC_SATA_INT_ERROR_M		= 0x2,
	VSC_SATA_INT_PHY_CHANGE		= 0x1,
	VSC_SATA_INT_ERROR = (VSC_SATA_INT_ERROR_CRC  | VSC_SATA_INT_ERROR_T | \
			      VSC_SATA_INT_ERROR_P    | VSC_SATA_INT_ERROR_R | \
			      VSC_SATA_INT_ERROR_E    | VSC_SATA_INT_ERROR_M | \
			      VSC_SATA_INT_PHY_CHANGE),
};


#define is_vsc_sata_int_err(port_idx, int_status) \
	 (int_status & (VSC_SATA_INT_ERROR << (8 * port_idx)))


static u32 vsc_sata_scr_read (struct ata_port *ap, unsigned int sc_reg)
{
	if (sc_reg > SCR_CONTROL)
		return 0xffffffffU;
	return readl((void __iomem *) ap->ioaddr.scr_addr + (sc_reg * 4));
}


static void vsc_sata_scr_write (struct ata_port *ap, unsigned int sc_reg,
			       u32 val)
{
	if (sc_reg > SCR_CONTROL)
		return;
	writel(val, (void __iomem *) ap->ioaddr.scr_addr + (sc_reg * 4));
}


static void vsc_intr_mask_update(struct ata_port *ap, u8 ctl)
{
	void __iomem *mask_addr;
	u8 mask;

	mask_addr = ap->host->mmio_base +
		VSC_SATA_INT_MASK_OFFSET + ap->port_no;
	mask = readb(mask_addr);
	if (ctl & ATA_NIEN)
		mask |= 0x80;
	else
		mask &= 0x7F;
	writeb(mask, mask_addr);
}


static void vsc_sata_tf_load(struct ata_port *ap, const struct ata_taskfile *tf)
{
	struct ata_ioports *ioaddr = &ap->ioaddr;
	unsigned int is_addr = tf->flags & ATA_TFLAG_ISADDR;

	/*
	 * The only thing the ctl register is used for is SRST.
	 * That is not enabled or disabled via tf_load.
	 * However, if ATA_NIEN is changed, then we need to change the interrupt register.
	 */
	if ((tf->ctl & ATA_NIEN) != (ap->last_ctl & ATA_NIEN)) {
		ap->last_ctl = tf->ctl;
		vsc_intr_mask_update(ap, tf->ctl & ATA_NIEN);
	}
	if (is_addr && (tf->flags & ATA_TFLAG_LBA48)) {
		writew(tf->feature | (((u16)tf->hob_feature) << 8),
		       (void __iomem *) ioaddr->feature_addr);
		writew(tf->nsect | (((u16)tf->hob_nsect) << 8),
		       (void __iomem *) ioaddr->nsect_addr);
		writew(tf->lbal | (((u16)tf->hob_lbal) << 8),
		       (void __iomem *) ioaddr->lbal_addr);
		writew(tf->lbam | (((u16)tf->hob_lbam) << 8),
		       (void __iomem *) ioaddr->lbam_addr);
		writew(tf->lbah | (((u16)tf->hob_lbah) << 8),
		       (void __iomem *) ioaddr->lbah_addr);
	} else if (is_addr) {
		writew(tf->feature, (void __iomem *) ioaddr->feature_addr);
		writew(tf->nsect, (void __iomem *) ioaddr->nsect_addr);
		writew(tf->lbal, (void __iomem *) ioaddr->lbal_addr);
		writew(tf->lbam, (void __iomem *) ioaddr->lbam_addr);
		writew(tf->lbah, (void __iomem *) ioaddr->lbah_addr);
	}

	if (tf->flags & ATA_TFLAG_DEVICE)
		writeb(tf->device, (void __iomem *) ioaddr->device_addr);

	ata_wait_idle(ap);
}


static void vsc_sata_tf_read(struct ata_port *ap, struct ata_taskfile *tf)
{
	struct ata_ioports *ioaddr = &ap->ioaddr;
	u16 nsect, lbal, lbam, lbah, feature;

	tf->command = ata_check_status(ap);
	tf->device = readw((void __iomem *) ioaddr->device_addr);
	feature = readw((void __iomem *) ioaddr->error_addr);
	nsect = readw((void __iomem *) ioaddr->nsect_addr);
	lbal = readw((void __iomem *) ioaddr->lbal_addr);
	lbam = readw((void __iomem *) ioaddr->lbam_addr);
	lbah = readw((void __iomem *) ioaddr->lbah_addr);

	tf->feature = feature;
	tf->nsect = nsect;
	tf->lbal = lbal;
	tf->lbam = lbam;
	tf->lbah = lbah;

	if (tf->flags & ATA_TFLAG_LBA48) {
		tf->hob_feature = feature >> 8;
		tf->hob_nsect = nsect >> 8;
		tf->hob_lbal = lbal >> 8;
		tf->hob_lbam = lbam >> 8;
		tf->hob_lbah = lbah >> 8;
        }
}


/*
 * vsc_sata_interrupt
 *
 * Read the interrupt register and process for the devices that have them pending.
 */
static irqreturn_t vsc_sata_interrupt (int irq, void *dev_instance)
{
	struct ata_host *host = dev_instance;
	unsigned int i;
	unsigned int handled = 0;
	u32 int_status;

	spin_lock(&host->lock);

	int_status = readl(host->mmio_base + VSC_SATA_INT_STAT_OFFSET);

	for (i = 0; i < host->n_ports; i++) {
		if (int_status & ((u32) 0xFF << (8 * i))) {
			struct ata_port *ap;

			ap = host->ports[i];

			if (is_vsc_sata_int_err(i, int_status)) {
				u32 err_status;
				printk(KERN_DEBUG "%s: ignoring interrupt(s)\n", __FUNCTION__);
				err_status = ap ? vsc_sata_scr_read(ap, SCR_ERROR) : 0;
				vsc_sata_scr_write(ap, SCR_ERROR, err_status);
				handled++;
			}

			if (ap && !(ap->flags & ATA_FLAG_DISABLED)) {
				struct ata_queued_cmd *qc;

				qc = ata_qc_from_tag(ap, ap->active_tag);
				if (qc && (!(qc->tf.flags & ATA_TFLAG_POLLING)))
					handled += ata_host_intr(ap, qc);
				else if (is_vsc_sata_int_err(i, int_status)) {
					/*
					 * On some chips (i.e. Intel 31244), an error
					 * interrupt will sneak in at initialization
					 * time (phy state changes).  Clearing the SCR
					 * error register is not required, but it prevents
					 * the phy state change interrupts from recurring
					 * later.
					 */
					u32 err_status;
					err_status = vsc_sata_scr_read(ap, SCR_ERROR);
					printk(KERN_DEBUG "%s: clearing interrupt, "
					       "status %x; sata err status %x\n",
					       __FUNCTION__,
					       int_status, err_status);
					vsc_sata_scr_write(ap, SCR_ERROR, err_status);
					/* Clear interrupt status */
					ata_chk_status(ap);
					handled++;
				}
			}
		}
	}

	spin_unlock(&host->lock);

	return IRQ_RETVAL(handled);
}


static struct scsi_host_template vsc_sata_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.ioctl			= ata_scsi_ioctl,
	.queuecommand		= ata_scsi_queuecmd,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= LIBATA_MAX_PRD,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.slave_destroy		= ata_scsi_slave_destroy,
	.bios_param		= ata_std_bios_param,
};


static const struct ata_port_operations vsc_sata_ops = {
	.port_disable		= ata_port_disable,
	.tf_load		= vsc_sata_tf_load,
	.tf_read		= vsc_sata_tf_read,
	.exec_command		= ata_exec_command,
	.check_status		= ata_check_status,
	.dev_select		= ata_std_dev_select,
	.bmdma_setup            = ata_bmdma_setup,
	.bmdma_start            = ata_bmdma_start,
	.bmdma_stop		= ata_bmdma_stop,
	.bmdma_status		= ata_bmdma_status,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.data_xfer		= ata_mmio_data_xfer,
	.freeze			= ata_bmdma_freeze,
	.thaw			= ata_bmdma_thaw,
	.error_handler		= ata_bmdma_error_handler,
	.post_internal_cmd	= ata_bmdma_post_internal_cmd,
	.irq_handler		= vsc_sata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,
	.scr_read		= vsc_sata_scr_read,
	.scr_write		= vsc_sata_scr_write,
	.port_start		= ata_port_start,
	.port_stop		= ata_port_stop,
	.host_stop		= ata_pci_host_stop,
};

static void __devinit vsc_sata_setup_port(struct ata_ioports *port, unsigned long base)
{
	port->cmd_addr		= base + VSC_SATA_TF_CMD_OFFSET;
	port->data_addr		= base + VSC_SATA_TF_DATA_OFFSET;
	port->error_addr	= base + VSC_SATA_TF_ERROR_OFFSET;
	port->feature_addr	= base + VSC_SATA_TF_FEATURE_OFFSET;
	port->nsect_addr	= base + VSC_SATA_TF_NSECT_OFFSET;
	port->lbal_addr		= base + VSC_SATA_TF_LBAL_OFFSET;
	port->lbam_addr		= base + VSC_SATA_TF_LBAM_OFFSET;
	port->lbah_addr		= base + VSC_SATA_TF_LBAH_OFFSET;
	port->device_addr	= base + VSC_SATA_TF_DEVICE_OFFSET;
	port->status_addr	= base + VSC_SATA_TF_STATUS_OFFSET;
	port->command_addr	= base + VSC_SATA_TF_COMMAND_OFFSET;
	port->altstatus_addr	= base + VSC_SATA_TF_ALTSTATUS_OFFSET;
	port->ctl_addr		= base + VSC_SATA_TF_CTL_OFFSET;
	port->bmdma_addr	= base + VSC_SATA_DMA_CMD_OFFSET;
	port->scr_addr		= base + VSC_SATA_SCR_STATUS_OFFSET;
	writel(0, (void __iomem *) base + VSC_SATA_UP_DESCRIPTOR_OFFSET);
	writel(0, (void __iomem *) base + VSC_SATA_UP_DATA_BUFFER_OFFSET);
}


static int __devinit vsc_sata_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	static int printed_version;
	struct ata_probe_ent *probe_ent = NULL;
	unsigned long base;
	int pci_dev_busy = 0;
	void __iomem *mmio_base;
	int rc;

	if (!printed_version++)
		dev_printk(KERN_DEBUG, &pdev->dev, "version " DRV_VERSION "\n");

	rc = pci_enable_device(pdev);
	if (rc)
		return rc;

	/*
	 * Check if we have needed resource mapped.
	 */
	if (pci_resource_len(pdev, 0) == 0) {
		rc = -ENODEV;
		goto err_out;
	}

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc) {
		pci_dev_busy = 1;
		goto err_out;
	}

	/*
	 * Use 32 bit DMA mask, because 64 bit address support is poor.
	 */
	rc = pci_set_dma_mask(pdev, DMA_32BIT_MASK);
	if (rc)
		goto err_out_regions;
	rc = pci_set_consistent_dma_mask(pdev, DMA_32BIT_MASK);
	if (rc)
		goto err_out_regions;

	probe_ent = kmalloc(sizeof(*probe_ent), GFP_KERNEL);
	if (probe_ent == NULL) {
		rc = -ENOMEM;
		goto err_out_regions;
	}
	memset(probe_ent, 0, sizeof(*probe_ent));
	probe_ent->dev = pci_dev_to_dev(pdev);
	INIT_LIST_HEAD(&probe_ent->node);

	mmio_base = pci_iomap(pdev, 0, 0);
	if (mmio_base == NULL) {
		rc = -ENOMEM;
		goto err_out_free_ent;
	}
	base = (unsigned long) mmio_base;

	/*
	 * Due to a bug in the chip, the default cache line size can't be used
	 */
	pci_write_config_byte(pdev, PCI_CACHE_LINE_SIZE, 0x80);

	probe_ent->sht = &vsc_sata_sht;
	probe_ent->port_flags = ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY |
				ATA_FLAG_MMIO;
	probe_ent->port_ops = &vsc_sata_ops;
	probe_ent->n_ports = 4;
	probe_ent->irq = pdev->irq;
	probe_ent->irq_flags = IRQF_SHARED;
	probe_ent->mmio_base = mmio_base;

	/* We don't care much about the PIO/UDMA masks, but the core won't like us
	 * if we don't fill these
	 */
	probe_ent->pio_mask = 0x1f;
	probe_ent->mwdma_mask = 0x07;
	probe_ent->udma_mask = 0x7f;

	/* We have 4 ports per PCI function */
	vsc_sata_setup_port(&probe_ent->port[0], base + 1 * VSC_SATA_PORT_OFFSET);
	vsc_sata_setup_port(&probe_ent->port[1], base + 2 * VSC_SATA_PORT_OFFSET);
	vsc_sata_setup_port(&probe_ent->port[2], base + 3 * VSC_SATA_PORT_OFFSET);
	vsc_sata_setup_port(&probe_ent->port[3], base + 4 * VSC_SATA_PORT_OFFSET);

	pci_set_master(pdev);

	/*
	 * Config offset 0x98 is "Extended Control and Status Register 0"
	 * Default value is (1 << 28).  All bits except bit 28 are reserved in
	 * DPA mode.  If bit 28 is set, LED 0 reflects all ports' activity.
	 * If bit 28 is clear, each port has its own LED.
	 */
	pci_write_config_dword(pdev, 0x98, 0);

	/* FIXME: check ata_device_add return value */
	ata_device_add(probe_ent);
	kfree(probe_ent);

	return 0;

err_out_free_ent:
	kfree(probe_ent);
err_out_regions:
	pci_release_regions(pdev);
err_out:
	if (!pci_dev_busy)
		pci_disable_device(pdev);
	return rc;
}

static const struct pci_device_id vsc_sata_pci_tbl[] = {
	{ PCI_VENDOR_ID_VITESSE, 0x7174,
	  PCI_ANY_ID, PCI_ANY_ID, 0x10600, 0xFFFFFF, 0 },
	{ PCI_VENDOR_ID_INTEL, 0x3200,
	  PCI_ANY_ID, PCI_ANY_ID, 0x10600, 0xFFFFFF, 0 },

	{ }	/* terminate list */
};

static struct pci_driver vsc_sata_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= vsc_sata_pci_tbl,
	.probe			= vsc_sata_init_one,
	.remove			= ata_pci_remove_one,
};

static int __init vsc_sata_init(void)
{
	return pci_register_driver(&vsc_sata_pci_driver);
}

static void __exit vsc_sata_exit(void)
{
	pci_unregister_driver(&vsc_sata_pci_driver);
}

MODULE_AUTHOR("Jeremy Higdon");
MODULE_DESCRIPTION("low-level driver for Vitesse VSC7174 SATA controller");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, vsc_sata_pci_tbl);
MODULE_VERSION(DRV_VERSION);

module_init(vsc_sata_init);
module_exit(vsc_sata_exit);
