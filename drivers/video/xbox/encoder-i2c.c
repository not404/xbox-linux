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
#define XLB_ADDRESS 0x70
#define EEPROM_ADDRESS 0x54
#define PIC_ADDRESS 0x10

#define DRIVER_NAME "xbox-tv-i2c"


#ifndef MODULE
extern int __init i2c_xbox_init(void);
#endif

static int tv_attach_adapter(struct i2c_adapter *adap);

static struct i2c_driver tv_driver = {
	.driver = {
		.name	= "i2c xbox tv_driver",
	},
	.attach_adapter	= tv_attach_adapter,
};

static struct i2c_client pic_client = {
	.name		= "I2C xbox pic",
	.flags		= 0,
	.addr		= PIC_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client conexant_client = {
	.name		= "I2C xbox conexant",
	.flags		= 0,
	.addr		= CONEXANT_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client focus_client = {
	.name		= "I2C xbox focus",
	.flags		= 0,
	.addr		= FOCUS_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client xcalibur_client = {
	.name		= "I2C xbox Xcalibur",
	.flags		= 0,
	.addr		= XLB_ADDRESS,
	.adapter	= NULL,
	.driver		= &tv_driver,
};

static struct i2c_client eeprom_client = {
	.name		= "I2C xbox eeprom",
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
	conexant_client.adapter = adap;
	focus_client.adapter = adap;
	xcalibur_client.adapter = adap;
	pic_client.adapter = adap;
	eeprom_client.adapter = adap;
	i2c_attach_client(&conexant_client);
	i2c_attach_client(&focus_client);
	i2c_attach_client(&xcalibur_client);
	i2c_attach_client(&pic_client);
	i2c_attach_client(&eeprom_client);

	return 0;
}

int tv_i2c_init(void) {
	int res;

#ifndef MODULE
	i2c_xbox_init();
#endif

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

int xcalibur_i2c_read_reg(unsigned char adr) {
	if (!xcalibur_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No Xcalibur client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_read_byte_data(&xcalibur_client, adr);
}

int xcalibur_i2c_read_block(unsigned char adr, unsigned char *data, int len) {
	if (!xcalibur_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No Xcalibur client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_read_i2c_block_data(&xcalibur_client, adr, len, data);
}

int xcalibur_i2c_write_block(unsigned char adr, unsigned char *data, int len){
	if (!xcalibur_client.adapter) {
		printk(KERN_ERR DRIVER_NAME " : No Xcalibur client attached.\n");
		return -1;
	}
	udelay(500);
	return i2c_smbus_write_block_data(&xcalibur_client, adr, len, data);
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
	i2c_del_driver(&tv_driver);
}

