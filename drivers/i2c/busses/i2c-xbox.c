/*
    i2c-xbox.c - Part of lm_sensors, Linux kernel modules for hardware
              monitoring

    Copyright (c) 1999-2002 Edgar Hucek <hostmaster@ed-soft.at>

    Shamelessly ripped from i2c-xbox.c:

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
   Supports XBOX, Note: we assume there can only be one device, with one SMBus interface.
*/

#include <linux/version.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/errno.h>  /* error codes */
#include <linux/interrupt.h> /* intr_count */
#include <linux/xbox.h>
#include <linux/delay.h>


#define GS_ABRT_STS (1 << 0)
#define GS_COL_STS (1 << 1)
#define GS_PRERR_STS (1 << 2)
#define GS_HST_STS (1 << 3)
#define GS_IRQ_ON (1 << 4)
#define GS_HCYC_STS (1 << 4)
#define GS_TO_STS (1 << 5)
#define GS_SMB_STS (1 << 11)

#define SMB_GCTL_HOST_START      (1 << 3)
#define SMB_GCTL_HOST_INTERRUPT  (1 << 4)
#define SMB_GCTL_ABORT           (1 << 5)
#define SMB_GCTL_SNOOP           (1 << 8)
#define SMB_GCTL_SLAVE_INTERRUPT (1 << 9)
#define SMB_GCTL_ALERT_INTERRUPT (1 << 10)

#define GS_CLEAR_STS (GS_ABRT_STS | GS_COL_STS | GS_PRERR_STS | \
  GS_HCYC_STS | GS_TO_STS)

#define GE_CYC_TYPE_MASK (7)
#define GE_HOST_STC (1 << 3)
#define GE_ABORT (1 << 5)

#define I2C_HW_SMBUS_XBOX   0x05
#define SMBGCFG   0x041          /* mh */
#define SMBBA     0x058           /* mh */

struct sd {
    const unsigned short vendor;
    const unsigned short device;
    const unsigned short function;
    const char* name;
    int amdsetup:1;
};

static struct sd supported[] = {
    {PCI_VENDOR_ID_NVIDIA, 0x01b4, 1, "nVidia XBOX nForce", 0},
    {0, 0, 0}
};

/* XBOX SMBus address offsets */
#define SMB_ADDR_OFFSET        0x04
#define SMB_IOSIZE             8
#define SMB_GLOBAL_STATUS      (0x0 + xbox_smba)
#define SMB_GLOBAL_ENABLE      (0x2 + xbox_smba)
#define SMB_HOST_ADDRESS       (0x4 + xbox_smba)
#define SMB_HOST_DATA          (0x6 + xbox_smba)
#define SMB_HOST_COMMAND       (0x8 + xbox_smba)





/* Other settings */
#define MAX_TIMEOUT 500

/* XBOX constants */
#define XBOX_QUICK        0x00
#define XBOX_BYTE         0x01
#define XBOX_BYTE_DATA    0x02
#define XBOX_WORD_DATA    0x03
#define XBOX_PROCESS_CALL 0x04
#define XBOX_BLOCK_DATA   0x05

/* insmod parameters */

#ifdef MODULE
static
#else
extern
#endif
int __init i2c_xbox_init(void);
static int __init xbox_cleanup(void);
static int xbox_setup(void);
static s32 xbox_access(struct i2c_adapter *adap, u16 addr,
			 unsigned short flags, char read_write,
			 u8 command, int size, union i2c_smbus_data *data);
static void xbox_do_pause(unsigned int amount);
static void xbox_abort(void);
static int xbox_transaction(void);
static u32 xbox_func(struct i2c_adapter *adapter);

static struct i2c_algorithm smbus_algorithm = {
	/* name */ "Non-I2C SMBus adapter",
	/* id */ I2C_ALGO_SMBUS,
	/* master_xfer */ NULL,
	/* smbus_access */ xbox_access,
	/* slave;_send */ NULL,
	/* slave_rcv */ NULL,
	/* algo_control */ NULL,
	/* functionality */ xbox_func,
};

static struct i2c_adapter xbox_adapter = {
	.owner          = THIS_MODULE,
	.class          = I2C_CLASS_HWMON,
	.algo           = &smbus_algorithm,
	.name           = "unset",
};

static int __initdata xbox_initialized;
static unsigned short xbox_smba = 0;
spinlock_t xbox_driver_lock = SPIN_LOCK_UNLOCKED;
struct driver_data;
static struct pci_dev *XBOX_dev;

/* Detect whether a XBOX can be found, and initialize it, where necessary.
   Note the differences between kernels with the old PCI BIOS interface and
   newer kernels with the real PCI interface. In compat.h some things are
   defined to make the transition easier. */
int xbox_setup(void)
{
	unsigned char temp;
	struct sd *currdev;
	u16 cmd;

	XBOX_dev = NULL;

	/* Look for a supported chip */
	for(currdev = supported; currdev->vendor; ) {
		XBOX_dev = pci_find_device(currdev->vendor,
						currdev->device, XBOX_dev);
		if (XBOX_dev != NULL)	{
	                pci_read_config_byte(XBOX_dev, SMBGCFG, &temp);
			pci_read_config_word(XBOX_dev, 0x14, &xbox_smba);
			
			xbox_smba &= 0xfffc;
			if (PCI_FUNC(XBOX_dev->devfn) == currdev->function)
			{
				pci_read_config_word(XBOX_dev,PCI_STATUS,&cmd);
				break;
			}
		} else {
		    currdev++;
		}
	}

	if (XBOX_dev == NULL) {
		printk
		    ("i2c-xbox.o: Error: No XBOX or compatible device detected!\n");
		return(-ENODEV);
	}
	printk(KERN_INFO "i2c-xbox.o: Found %s SMBus controller.\n", currdev->name);

	/* Everything is happy, let's grab the memory and set things up. */
	if(!request_region(xbox_smba, SMB_IOSIZE, "xbox-smbus")) {
		printk
		    ("i2c-xbox.o: SMB region 0x%x already in use!\n",
		     xbox_smba);
		return(-ENODEV);
	}

	return 0;
}

/* 
  SMBUS event = I/O 28-29 bit 11
     see E0 for the status bits and enabled in E2
     
*/

/* Internally used pause function */
void xbox_do_pause(unsigned int amount)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(amount);
}

void xbox_abort(void)
{
	printk("i2c-xbox.o: Sending abort.\n");
	outw_p(inw(SMB_GLOBAL_ENABLE) | GE_ABORT, SMB_GLOBAL_ENABLE);
	xbox_do_pause(100);
	outw_p(GS_CLEAR_STS, SMB_GLOBAL_STATUS);
}

int xbox_transaction(void)
{
	int temp;
	int result = 0;
	int timeout = 0;

	/* Make sure the SMBus host is ready to start transmitting */
	if ((temp = inw_p(SMB_GLOBAL_STATUS)) & (GS_HST_STS | GS_SMB_STS)) {
		do {
			udelay(100);
			temp = inw_p(SMB_GLOBAL_STATUS);
		} while ((temp & (GS_HST_STS | GS_SMB_STS)) &&
		         (timeout++ < MAX_TIMEOUT));
		/* If the SMBus is still busy, we give up */
		if (timeout >= MAX_TIMEOUT) {
			printk("i2c-xbox.o: Busy wait timeout! (%04x)\n", temp);
			xbox_abort();
			return(-1);
		}
		timeout = 0;
	}

	/* start the transaction by setting the start bit */
	outw_p(inw(SMB_GLOBAL_ENABLE) | GE_HOST_STC , SMB_GLOBAL_ENABLE);

	/* We will always wait for a fraction of a second! */
	temp = inw_p(SMB_GLOBAL_STATUS);
	while ((temp & GS_HST_STS) && (timeout++ < MAX_TIMEOUT)) {
		udelay(100);
		temp = inw_p(SMB_GLOBAL_STATUS);		
	} 

	/* If the SMBus is still busy, we give up */
	if (timeout >= MAX_TIMEOUT) {
		printk("i2c-xbox.o: Completion timeout!\n");
		xbox_abort ();
		return(-1);
	}

	if (temp & GS_PRERR_STS) {
		result = -1;
	}

	if (temp & GS_COL_STS) {
		result = -1;
		printk("i2c-xbox.o: SMBus collision!\n");
	}

	if (temp & GS_TO_STS) {
		result = -1;
	}
	outw_p(GS_CLEAR_STS, SMB_GLOBAL_STATUS);

	return result;
}

/* Return -1 on error. */
s32 xbox_access(struct i2c_adapter * adap, u16 addr,
		  unsigned short flags, char read_write,
		  u8 command, int size, union i2c_smbus_data * data)
{
  /** TODO: Should I supporte the 10-bit transfers? */
	switch (size) {
	case I2C_SMBUS_PROC_CALL:
		printk
		    ("i2c-xbox.o: I2C_SMBUS_PROC_CALL not supported!\n");
		/* TODO: Well... It is supported, I'm just not sure what to do here... */
		return -1;
	case I2C_SMBUS_QUICK:
		outw_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMB_HOST_ADDRESS);
		size = XBOX_QUICK;
		break;
	case I2C_SMBUS_BYTE:
		outw_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMB_HOST_ADDRESS);
		/* TODO: Why only during write? */
		if (read_write == I2C_SMBUS_WRITE)
			outb_p(command, SMB_HOST_COMMAND);
		size = XBOX_BYTE;
		break;
	case I2C_SMBUS_BYTE_DATA:
		outw_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMB_HOST_ADDRESS);
		outb_p(command, SMB_HOST_COMMAND);
		if (read_write == I2C_SMBUS_WRITE)
			outw_p(data->byte, SMB_HOST_DATA);
		size = XBOX_BYTE_DATA;
		break;
	case I2C_SMBUS_WORD_DATA:
		outw_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMB_HOST_ADDRESS);
		outb_p(command, SMB_HOST_COMMAND);
		if (read_write == I2C_SMBUS_WRITE)
			outw_p(data->word, SMB_HOST_DATA);	/* TODO: endian???? */
		size = XBOX_WORD_DATA;
		break;
	}

	/* How about enabling interrupts... */
	outw_p(size & GE_CYC_TYPE_MASK, SMB_GLOBAL_ENABLE);

	if (xbox_transaction())	/* Error in transaction */
		return -1;

	if ((read_write == I2C_SMBUS_WRITE) || (size == XBOX_QUICK))
		return 0;


	switch (size) {
	case XBOX_BYTE:
		data->byte = inw_p(SMB_HOST_DATA);
		break;
	case XBOX_BYTE_DATA:
		data->byte = inw_p(SMB_HOST_DATA);
		break;
	case XBOX_WORD_DATA:
		data->word = inw_p(SMB_HOST_DATA);	/* TODO: endian???? */
		break;
	}

	return 0;
}

u32 xbox_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
	    I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	    I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_PROC_CALL;
}

int __init i2c_xbox_init(void)
{
	int res;
	printk("i2c-xbox.o version 0.0.1\n");
	xbox_initialized = 0;
	if ((res = xbox_setup())) {
		printk
		    ("i2c-xbox.o: XBOX or compatible device not detected, module not inserted.\n");
		xbox_cleanup();
		return res;
	}
	xbox_initialized++;
	sprintf(xbox_adapter.name, "SMBus adapter at %04x",xbox_smba);
	if ((res = i2c_add_adapter(&xbox_adapter))) {
		printk
		    ("i2c-xbox.o: Adapter registration failed, module not inserted.\n");
		xbox_cleanup();
		return res;
	}
	xbox_initialized++;
	printk("i2c-xbox.o: SMBus bus detected and initialized\n");
	return 0;
}

int __init xbox_cleanup(void)
{
	int res;
	if (xbox_initialized >= 2) {
		if ((res = i2c_del_adapter(&xbox_adapter))) {
			printk
			    ("i2c-xbox.o: i2c_del_adapter failed, module not removed\n");
			return res;
		} else
			xbox_initialized--;
	}
	if (xbox_initialized >= 1) {
		release_region(xbox_smba, SMB_IOSIZE);
		xbox_initialized--;
	}
	free_irq(XBOX_dev->irq, XBOX_dev);
	return 0;
}

EXPORT_SYMBOL(i2c_xbox_init);

#ifdef MODULE

MODULE_AUTHOR("Edgar Hucek <hostmaster@ed-soft.at>");
MODULE_DESCRIPTION("XBOX nForce SMBus driver");

#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

int init_module(void)
{
	return i2c_xbox_init();
}

void cleanup_module(void)
{
	xbox_cleanup();
}

#endif				/* MODULE */
