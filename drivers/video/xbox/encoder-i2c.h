/*
 * linux/drivers/video/riva/encoder-i2c.h - Xbox I2C driver for encoder chip
 *
 * Maintainer: Oliver Schwartz <Oliver.Schwartz@gmx.de>
 *
 * Contributors:
 * 
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Known bugs and issues:
 *
 *      none
 */
 
#ifndef encoder_i2c_h
#define encoder_i2c_h

int tv_i2c_init(void);
void tv_i2c_exit(void);
int conexant_i2c_read_reg(unsigned char adr);
int conexant_i2c_write_reg(unsigned char adr, unsigned char value);
int focus_i2c_read_reg(unsigned char adr);
int focus_i2c_write_reg(unsigned char adr, unsigned char value);
unsigned char pic_i2c_read_reg(unsigned char adr);
unsigned char eeprom_i2c_read(unsigned char adr);

#endif
