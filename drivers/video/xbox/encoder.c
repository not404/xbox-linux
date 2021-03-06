/*
 * linux/drivers/video/riva/encoder.c - Xbox driver for encoder chip
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

#include "encoder-i2c.h"
#include "encoder.h"
#include "focus.h"
#include <asm/io.h>

xbox_encoder_type tv_get_video_encoder(void) {
	unsigned char b = 0;

	b = conexant_i2c_read_reg(0x00);
	if(b != 255) {
		return ENCODER_CONEXANT;
	}
	b = focus_i2c_read_reg(0x00);
	if(b != 255) {
		return ENCODER_FOCUS;
	}
	b = xcalibur_i2c_read_reg(0x00);
	if(b != 255) {
		return ENCODER_XCALIBUR;
	}
	return 0;
}

int tv_init(void) {
	return tv_i2c_init();
}

void tv_exit(void) {
	tv_i2c_exit();
}

void tv_load_mode(unsigned char *encoder_regs) {
	int n, n1;
	unsigned char b;
	unsigned char *mode;
	u32 *XCal_Reg;
	switch (tv_get_video_encoder()) {
		case ENCODER_CONEXANT:
			conexant_i2c_write_reg(0xc4, 0x00); // EN_OUT = 1
			mode = (unsigned char *)encoder_regs;
			// Conexant init (starts at register 0x2e)
			n1=0;
			for(n=0x2e;n<0x100;n+=2) {
				switch(n) {
					case 0x6c: // reset
						conexant_i2c_write_reg(n, mode[n1] & 0x7f);
						break;
					case 0xc4: // EN_OUT
						conexant_i2c_write_reg(n, mode[n1] & 0xfe);
						break;
					case 0xb8: // autoconfig
						break;
	
					default:
						conexant_i2c_write_reg(n, mode[n1]);
						break;
				}
				n1++;
			}
			// Timing Reset
			b=conexant_i2c_read_reg(0x6c) & (0x7f);
			conexant_i2c_write_reg(0x6c, 0x80|b);
			b=conexant_i2c_read_reg(0xc4) & (0xfe);
			conexant_i2c_write_reg(0xc4, 0x01|b); // EN_OUT = 1
			
			conexant_i2c_write_reg(0xA8, 0x81);
			conexant_i2c_write_reg(0xAA, 0x49);
			conexant_i2c_write_reg(0xAC, 0x8C);
			break;
		case ENCODER_FOCUS:
			mode = (unsigned char *)encoder_regs;
			//Set the command register soft reset
			focus_i2c_write_reg(0x0c,0x03);
			focus_i2c_write_reg(0x0d,0x21);
			
			for (n = 0; n<0xc4; n++) {
				focus_i2c_write_reg(n,mode[n]);
			}
			//Clear soft reset flag
			b = focus_i2c_read_reg(0x0c);
			b &= ~0x01;
			focus_i2c_write_reg(0x0c,b);
			b = focus_i2c_read_reg(0x0d);
			focus_i2c_write_reg(0x0d,b);
			break;
		case ENCODER_XCALIBUR:
			//Xcalibur regs are 4 bytes long
			XCal_Reg = (u32*)encoder_regs;
			mode = kmalloc(4*sizeof(char),GFP_KERNEL);
			for(n = 0; n < 0x90; n++) {
				//Endianness.
				memcpy(&mode[0],(unsigned char*)(&XCal_Reg[n])+3,0x01);
				memcpy(&mode[1],(unsigned char*)(&XCal_Reg[n])+2,0x01);
				memcpy(&mode[2],(unsigned char*)(&XCal_Reg[n])+1,0x01);
				memcpy(&mode[3],(unsigned char*)(&XCal_Reg[n]),0x01);
				xcalibur_i2c_write_block(n, mode, 0x04);
			}
						
			kfree(mode);
		}
	kfree(encoder_regs);
}

void tv_save_mode(unsigned char *encoder_regs) {
	int n, n1;
	char *mode;
	
	switch (tv_get_video_encoder()) {
		case ENCODER_CONEXANT:
			encoder_regs = kmalloc(256*sizeof(char),GFP_KERNEL);
			mode = (unsigned char*) encoder_regs;
			// Conexant init (starts at register 0x2e)
			n1=0;		
			for(n=0x2e;n<0x100;n+=2) {
				mode[n1] = conexant_i2c_read_reg(n);
				n1++;
			}
			break;
		case ENCODER_FOCUS:
			encoder_regs = kmalloc(256*sizeof(char),GFP_KERNEL);
			mode = (unsigned char*) encoder_regs;
			for (n=0;n<0xc4;n++) {
				mode[n] = focus_i2c_read_reg(n);
			}
			break;
		case ENCODER_XCALIBUR:
			encoder_regs = kmalloc(0x90*sizeof(char)*4, GFP_KERNEL);
			//We don't save these yet - sorry!
			break;
	}			
}

xbox_tv_encoding get_tv_encoding(void) {
	unsigned char eeprom_value;
	xbox_tv_encoding enc = TV_ENC_PALBDGHI;
	eeprom_value = eeprom_i2c_read(0x5a);
	if (eeprom_value == 0x40) {
		enc = TV_ENC_NTSC;
	}
	else {
		enc = TV_ENC_PALBDGHI;
	}
	return enc;
}

xbox_av_type detect_av_type(void) {
	xbox_av_type avType;
	switch (pic_i2c_read_reg(0x04)) {
		case 0: avType = AV_SCART_RGB; break;
		case 1: avType = AV_HDTV; break;
		case 2: avType = AV_VGA_SOG; break;
		case 4: avType = AV_SVIDEO; break;
		case 6: avType = AV_COMPOSITE; break;
		case 7: avType = AV_VGA; break;
		default: avType = AV_COMPOSITE; break;
	}
	return avType;
}

