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

#define ADR(x) (x / 2 - 0x17)

static const conexant_video_parameter vidstda[] = {
	{ 3579545.00, 0.0000053, 0.00000782, 0.0000047, 0.000063555, 0.0000094, 0.000035667, 0.0000015, 243, 262.5, 0.0000092 },
	{ 3579545.00, 0.0000053, 0.00000782, 0.0000047, 0.000064000, 0.0000094, 0.000035667, 0.0000015, 243, 262.5, 0.0000092 },
	{ 4433618.75, 0.0000056, 0.00000785, 0.0000047, 0.000064000, 0.0000105, 0.000036407, 0.0000015, 288, 312.5, 0.0000105 },
	{ 4433618.75, 0.0000056, 0.00000785, 0.0000047, 0.000064000, 0.0000094, 0.000035667, 0.0000015, 288, 312.5, 0.0000092 },
	{ 3582056.25, 0.0000056, 0.00000811, 0.0000047, 0.000064000, 0.0000105, 0.000036407, 0.0000015, 288, 312.5, 0.0000105 },
	{ 3575611.88, 0.0000058, 0.00000832, 0.0000047, 0.000063555, 0.0000094, 0.000035667, 0.0000015, 243, 262.5, 0.0000092 },
	{ 4433619.49, 0.0000053, 0.00000755, 0.0000047, 0.000063555, 0.0000105, 0.000036407, 0.0000015, 243, 262.5, 0.0000092 }
};


static const double pll_base = 13.5e6;

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
	b = xlb_i2c_read_reg(0x00);
	if(b != 255) {
		return ENCODER_XLB;
	}
	return 0;
}

int tv_init(void) {
	return tv_i2c_init();
}

void tv_exit(void) {
	tv_i2c_exit();
}

void tv_load_mode(unsigned char * mode) {
	int n, n1;
	unsigned char b;
	int encoder = 0;
	
	encoder = tv_get_video_encoder();

	if(encoder == ENCODER_CONEXANT) {
		conexant_i2c_write_reg(0xc4, 0x00); // EN_OUT = 1

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
		
		/*
		conexant_i2c_write_reg(0xA8, (0xD9/1.3));
		conexant_i2c_write_reg(0xAA, (0x9A/1.3));
		conexant_i2c_write_reg(0xAC, (0xA4/1.3));
		*/
		
		conexant_i2c_write_reg(0xA8, 0x81);
		conexant_i2c_write_reg(0xAA, 0x49);
		conexant_i2c_write_reg(0xAC, 0x8C);
		
	} 
	else if(encoder == ENCODER_FOCUS) {
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
	}
	else if (encoder == ENCODER_XLB) {

	}
}

void tv_save_mode(unsigned char * mode) {
	int n, n1;
	int encoder = 0;

	encoder = tv_get_video_encoder();

	if(encoder == ENCODER_CONEXANT) {
		// Conexant init (starts at register 0x2e)
		n1=0;
		for(n=0x2e;n<0x100;n+=2) {
			mode[n1] = conexant_i2c_read_reg(n);
			n1++;
		}
	} 
	else if (encoder == ENCODER_FOCUS) {
		for (n=0;n<0xc4;n++) {
			mode[n] = focus_i2c_read_reg(n);
		}

	}
	else if (encoder == ENCODER_XLB) {

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

