/*
 * linux/drivers/video/riva/encoder-i2c.c - Xbox I2C driver for encoder chip
 *
 * Maintainer: Oliver Schwartz <Oliver.Schwartz@gmx.de>
 *
 * Contributors:
 *
 * Most of the code was stolen from extsmi.c
 * 
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Known bugs and issues:
 *
 *      none
 */

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/delay.h>

#define CONEXANT_ADDRESS 0x45
#define FOCUS_ADDRESS 0x6a
#define EEPROM_ADDRESS 0x54
#define PIC_ADDRESS 0x10

#define DRIVER_NAME "xbox-tv-i2c"

extern int __init i2c_xbox_init(void);

static int tv_attach_adapter(struct i2c_adapter *adap);

static struct i2c_driver tv_driver = {
	.name		= "i2c xbox conexant driver",
	.id		= I2C_DRIVERID_I2CDEV,
	.flags		= I2C_DF_NOTIFY,
	.attach_adapter	= tv_attach_adapter,
};

static struct i2c_client pic_client = {
	.name		= "I2C xbox pic client",
	.id		= 2,
	.flags		= 0,
	.addr		= PIC_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client conexant_client = {
	.name		= "I2C xbox conexant client",
	.id		= 1,
	.flags		= 0,
	.addr		= CONEXANT_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client focus_client = {
	.name		= "I2C xbox focus client",
	.id		= 1,
	.flags		= 0,
	.addr		= FOCUS_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client eeprom_client = {
	.name		= "I2C xbox eeprom client",
	.id		= 3,
	.flags		= 0,
	.addr		= EEPROM_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static int tv_attach_adapter(struct i2c_adapter *adap)
{
	int i;

	if ((i = i2c_adapter_id(adap)) < 0) {
		printk("i2c-dev.o: Unknown adapter ?!?\n");
		return -ENODEV;
	}

	printk(KERN_INFO DRIVER_NAME ": Using '%s'!\n",adap->name);
	focus_client.adapter = adap;
	conexant_client.adapter = adap;
	pic_client.adapter = adap;
	eeprom_client.adapter = adap;
	i2c_attach_client(&focus_client);
	i2c_attach_client(&conexant_client);
	i2c_attach_client(&pic_client);
	i2c_attach_client(&eeprom_client);

	return 0;
}

int tv_i2c_init(void) {
	int res;
	i2c_xbox_init();
	if ((res = i2c_add_driver(&tv_driver))) {
		printk(KERN_ERR DRIVER_NAME ": XBox tv driver registration failed.\n");
		return res;
	}
	return 0;
}

int conexant_i2c_read_reg(unsigned char adr) {
	if (!conexant_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No conexant client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_read_byte_data(&conexant_client, adr);
}

int conexant_i2c_write_reg(unsigned char adr, unsigned char value) {
	if (!conexant_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No conexant client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_write_byte_data(&conexant_client, adr, value);
}

int focus_i2c_read_reg(unsigned char adr) {
	if (!focus_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No focus client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_read_byte_data(&focus_client, adr);
}

int focus_i2c_write_reg(unsigned char adr, unsigned char value) {
	if (!focus_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No focus client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_write_byte_data(&focus_client, adr, value);
}

unsigned char pic_i2c_read_reg(unsigned char adr) {
	if (!pic_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No pic client attached.\n");
		return 0;
	}
	udelay(500);
	return (unsigned char)i2c_smbus_read_byte_data(&pic_client, adr);
}

unsigned char eeprom_i2c_read(unsigned char adr) {
	if (!eeprom_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No eeprom client attached.\n");
		return 0;
	}
	udelay(500);
	return (unsigned char)i2c_smbus_read_byte_data(&eeprom_client, adr);
}

void tv_i2c_exit(void){
	int res;
	
	if ((res = i2c_del_driver(&tv_driver))) {
		printk(KERN_ERR DRIVER_NAME ": XBox tv Driver deregistration failed, "
		       "module not removed.\n");
	}
	return;
}

