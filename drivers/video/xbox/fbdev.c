/*
 * linux/drivers/video/riva/fbdev.c - nVidia Xbox fb driver
 *
 * Maintained by Oliver Schwartz <Oliver.Schwartz@gmx.de>
 *
 * Based on the nVidia RIVA 128/TNT/TNT2 fb driver, maintained by 
 * Ani Joshi <ajoshi@shell.unixbox.com>
 *
 * Copyright 1999-2000 Jeff Garzik
 *
 * Contributors:
 *
 *	Ani Joshi:  Lots of debugging and cleanup work, really helped
 *	get the driver going
 *
 *	Ferenc Bakonyi:  Bug fixes, cleanup, modularization
 *
 *	Jindrich Makovicka:  Accel code help, hw cursor, mtrr
 *
 *	Paul Richards:  Bug fixes, updates
 *
 * Initial template from skeletonfb.c, created 28 Dec 1997 by Geert Uytterhoeven
 * Includes riva_hw.c from nVidia, see copyright below.
 * KGI code provided the basis for state storage, init, and mode switching.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Known bugs and issues:
 *	restoring text mode fails
 *	doublescan modes are broken
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/pci.h>
#ifdef CONFIG_MTRR
#include <asm/mtrr.h>
#endif
#ifdef CONFIG_PPC_OF
#include <asm/prom.h>
#include <asm/pci-bridge.h>
#endif

#include "xboxfb.h"
#include "nvreg.h"
#include <linux/sys.h>
#include <asm/uaccess.h>
#include <linux/xboxfbctl.h>
#include "encoder-i2c.h"
#include "conexant.h"
#include "focus.h"
#include "xcalibur.h"

#ifndef CONFIG_PCI		/* sanity check */
#error This driver requires PCI support.
#endif

/* version number of this driver */
#define XBOXFB_VERSION "0.9.5b-xbox"

/* ------------------------------------------------------------------------- *
 *
 * various helpful macros and constants
 *
 * ------------------------------------------------------------------------- */
#ifdef CONFIG_FB_RIVA_DEBUG
#define NVTRACE          printk
#else
#define NVTRACE          if(0) printk
#endif

#define NVTRACE_ENTER(...)  NVTRACE("%s START\n", __FUNCTION__)
#define NVTRACE_LEAVE(...)  NVTRACE("%s END\n", __FUNCTION__)

#ifdef CONFIG_FB_RIVA_DEBUG
#define assert(expr) \
	if(!(expr)) { \
	printk( "Assertion failed! %s,%s,%s,line=%d\n",\
	#expr,__FILE__,__FUNCTION__,__LINE__); \
	BUG(); \
	}
#else
#define assert(expr)
#endif

#define PFX "xboxfb: "

/* macro that allows you to set overflow bits */
#define SetBitField(value,from,to) SetBF(to,GetBF(value,from))
#define SetBit(n)		(1<<(n))
#define Set8Bits(value)		((value)&0xff)

/* HW cursor parameters */
#define MAX_CURS		32

/* ------------------------------------------------------------------------- *
 *
 * prototypes
 *
 * ------------------------------------------------------------------------- */

static int xboxfb_blank(int blank, struct fb_info *info);

/* ------------------------------------------------------------------------- *
 *
 * card identification
 *
 * ------------------------------------------------------------------------- */

enum riva_chips {
	CH_GEFORCE3_XBOX
};

/* directly indexed by riva_chips enum, above */
static struct riva_chip_info {
	const char *name;
	unsigned arch_rev;
} riva_chip_info[] __initdata = {
        { "GeForce3", NV_ARCH_20}
};

static struct pci_device_id xboxfb_pci_tbl[] = {
	{ PCI_VENDOR_ID_NVIDIA, 0x2a0,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, CH_GEFORCE3_XBOX },
	{ 0, } /* terminate list */
};
MODULE_DEVICE_TABLE(pci, xboxfb_pci_tbl);

/* ------------------------------------------------------------------------- *
 *
 * global variables
 *
 * ------------------------------------------------------------------------- */

/* command line data, set in xboxfb_setup() */
static u32 pseudo_palette[17];
static int flatpanel __initdata = -1; /* Autodetect later */
static int forceCRTC __initdata = -1;
static int noaccel   __initdata = 0;
#ifdef CONFIG_MTRR
static int nomtrr __initdata = 0;
#endif

static char *mode_option __initdata = NULL;
static int  strictmode __initdata = 0;

static xbox_tv_encoding tv_encoding  __initdata = TV_ENC_INVALID;
static xbox_av_type av_type __initdata = AV_INVALID;
static int hoc __initdata = -1;
static int voc __initdata = -1;

static struct fb_fix_screeninfo xboxfb_fix = {
	.id		= "Xbox",
	.type		= FB_TYPE_PACKED_PIXELS,
	.xpanstep	= 1,
	.ypanstep	= 1,
};

static struct fb_var_screeninfo xboxfb_default_var = {
	.xres		= 640,
	.yres		= 480,
	.xres_virtual	= 640,
	.yres_virtual	= 480,
	.bits_per_pixel	= 8,
	.red		= {0, 8, 0},
	.green		= {0, 8, 0},
	.blue		= {0, 8, 0},
	.transp		= {0, 0, 0},
	.activate	= FB_ACTIVATE_NOW,
	.height		= -1,
	.width		= -1,
	.accel_flags	= FB_ACCELF_TEXT,
	.pixclock	= 39721,
	.left_margin	= 40,
	.right_margin	= 24,
	.upper_margin	= 32,
	.lower_margin	= 11,
	.hsync_len	= 96,
	.vsync_len	= 2,
	.vmode		= FB_VMODE_NONINTERLACED
};

static struct fb_var_screeninfo xboxfb_mode_480p = {
	.xres           = 720,
	.yres           = 480,
	.xres_virtual   = 720,
	.yres_virtual   = 480,
	.bits_per_pixel = 32,
	.red            = {0, 8, 16},
	.green          = {0, 8, 8},
	.blue           = {0, 8, 0},
	.transp         = {0, 0, 0},
	.activate       = FB_ACTIVATE_NOW,
	.height         = -1,
	.width          = -1,
	.accel_flags    = FB_ACCELF_TEXT,
	.pixclock       = 37000,
	.left_margin    = 56,
	.right_margin   = 18,
	.upper_margin   = 29,
	.lower_margin   = 9,
	.hsync_len      = 64,
	.vsync_len      = 7,
	.vmode          = FB_VMODE_NONINTERLACED
};

static struct fb_var_screeninfo xboxfb_mode_720p = {
	.xres           = 1280,
	.yres           = 720,
	.xres_virtual   = 1280,
	.yres_virtual   = 720,
	.bits_per_pixel = 8,
	.red            = {0, 8, 0},
	.green          = {0, 8, 0},
	.blue           = {0, 8, 0},
	.transp         = {0, 0, 0},
	.activate       = FB_ACTIVATE_NOW,
	.height         = -1,
	.width          = -1,
	.accel_flags    = FB_ACCELF_TEXT,
	.pixclock       = 13468,
	.left_margin    = 220,
	.right_margin   = 70,
	.upper_margin   = 22,
	.lower_margin   = 3,
	.hsync_len      = 80,
	.vsync_len      = 5,
	.vmode          = FB_VMODE_NONINTERLACED
};

static const char* tvEncodingNames[] = {
        "NTSC",
        "NTSC-60",
        "PAL-BDGHI",
        "PAL-N",
        "PAL-NC",
        "PAL-M",
        "PAL-60"
};

static const char* avTypeNames[] = {
        "SCART (RGB)",
        "S-Video",
        "VGA (Sync on green)",
        "HDTV (Component video)",
        "Composite",
        "VGA (internal syncs)"
};

/* from GGI */
static const struct riva_regs reg_template = {
	{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,	/* ATTR */
	 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	 0x41, 0x01, 0x0F, 0x00, 0x00},
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* CRT  */
	 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE3,	/* 0x10 */
	 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 0x20 */
	 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 0x30 */
	 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	 0x00,							/* 0x40 */
	 },
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,	/* GRA  */
	 0xFF},
	{0x03, 0x01, 0x0F, 0x00, 0x0E},				/* SEQ  */
	0xEB							/* MISC */
};

/*
 * Backlight control
 */
#ifdef CONFIG_PMAC_BACKLIGHT

static int riva_backlight_levels[] = {
    0x158,
    0x192,
    0x1c6,
    0x200,
    0x234,
    0x268,
    0x2a2,
    0x2d6,
    0x310,
    0x344,
    0x378,
    0x3b2,
    0x3e6,
    0x41a,
    0x454,
    0x534,
};

static int riva_set_backlight_enable(int on, int level, void *data);
static int riva_set_backlight_level(int level, void *data);
static struct backlight_controller riva_backlight_controller = {
	riva_set_backlight_enable,
	riva_set_backlight_level
};
#endif /* CONFIG_PMAC_BACKLIGHT */

/* ------------------------------------------------------------------------- *
 *
 * MMIO access macros
 *
 * ------------------------------------------------------------------------- */

static inline void CRTCout(struct riva_par *par, unsigned char index,
			   unsigned char val)
{
	VGA_WR08(par->riva.PCIO, 0x3d4, index);
	VGA_WR08(par->riva.PCIO, 0x3d5, val);
}

static inline unsigned char CRTCin(struct riva_par *par,
				   unsigned char index)
{
	VGA_WR08(par->riva.PCIO, 0x3d4, index);
	return (VGA_RD08(par->riva.PCIO, 0x3d5));
}

static inline void GRAout(struct riva_par *par, unsigned char index,
			  unsigned char val)
{
	VGA_WR08(par->riva.PVIO, 0x3ce, index);
	VGA_WR08(par->riva.PVIO, 0x3cf, val);
}

static inline unsigned char GRAin(struct riva_par *par,
				  unsigned char index)
{
	VGA_WR08(par->riva.PVIO, 0x3ce, index);
	return (VGA_RD08(par->riva.PVIO, 0x3cf));
}

static inline void SEQout(struct riva_par *par, unsigned char index,
			  unsigned char val)
{
	VGA_WR08(par->riva.PVIO, 0x3c4, index);
	VGA_WR08(par->riva.PVIO, 0x3c5, val);
}

static inline unsigned char SEQin(struct riva_par *par,
				  unsigned char index)
{
	VGA_WR08(par->riva.PVIO, 0x3c4, index);
	return (VGA_RD08(par->riva.PVIO, 0x3c5));
}

static inline void ATTRout(struct riva_par *par, unsigned char index,
			   unsigned char val)
{
	VGA_WR08(par->riva.PCIO, 0x3c0, index);
	VGA_WR08(par->riva.PCIO, 0x3c0, val);
}

static inline unsigned char ATTRin(struct riva_par *par,
				   unsigned char index)
{
	VGA_WR08(par->riva.PCIO, 0x3c0, index);
	return (VGA_RD08(par->riva.PCIO, 0x3c1));
}

static inline void MISCout(struct riva_par *par, unsigned char val)
{
	VGA_WR08(par->riva.PVIO, 0x3c2, val);
}

static inline unsigned char MISCin(struct riva_par *par)
{
	return (VGA_RD08(par->riva.PVIO, 0x3cc));
}

static u8 byte_rev[256] = {
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
	0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
	0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
	0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
	0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
	0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
	0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
	0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
	0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
	0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
	0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
	0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
	0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
	0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
	0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
	0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
	0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};

static inline void reverse_order(u32 *l)
{
	u8 *a = (u8 *)l;
	*a = byte_rev[*a], a++;
	*a = byte_rev[*a], a++;
	*a = byte_rev[*a], a++;
	*a = byte_rev[*a];
}

/* ------------------------------------------------------------------------- *
 *
 * cursor stuff
 *
 * ------------------------------------------------------------------------- */

/**
 * xboxfb_load_cursor_image - load cursor image to hardware
 * @data: address to monochrome bitmap (1 = foreground color, 0 = background)
 * @par:  pointer to private data
 * @w:    width of cursor image in pixels
 * @h:    height of cursor image in scanlines
 * @bg:   background color (ARGB1555) - alpha bit determines opacity
 * @fg:   foreground color (ARGB1555)
 *
 * DESCRIPTiON:
 * Loads cursor image based on a monochrome source and mask bitmap.  The
 * image bits determines the color of the pixel, 0 for background, 1 for
 * foreground.  Only the affected region (as determined by @w and @h 
 * parameters) will be updated.
 *
 * CALLED FROM:
 * xboxfb_cursor()
 */
static void xboxfb_load_cursor_image(struct riva_par *par, u8 *data8,
				     u16 bg, u16 fg, u32 w, u32 h)
{
	int i, j, k = 0;
	u32 b, tmp;
	u32 *data = (u32 *)data8;
	bg = le16_to_cpu(bg);
	fg = le16_to_cpu(fg);

	w = (w + 1) & ~1;

	for (i = 0; i < h; i++) {
		b = *data++;
		reverse_order(&b);
		
		for (j = 0; j < w/2; j++) {
			tmp = 0;
#if defined (__BIG_ENDIAN)
			tmp = (b & (1 << 31)) ? fg << 16 : bg << 16;
			b <<= 1;
			tmp |= (b & (1 << 31)) ? fg : bg;
			b <<= 1;
#else
			tmp = (b & 1) ? fg : bg;
			b >>= 1;
			tmp |= (b & 1) ? fg << 16 : bg << 16;
			b >>= 1;
#endif
			writel(tmp, &par->riva.CURSOR[k++]);
		}
		k += (MAX_CURS - w)/2;
	}
}

/* ------------------------------------------------------------------------- *
 *
 * general utility functions
 *
 * ------------------------------------------------------------------------- */

/**
 * riva_wclut - set CLUT entry
 * @chip: pointer to RIVA_HW_INST object
 * @regnum: register number
 * @red: red component
 * @green: green component
 * @blue: blue component
 *
 * DESCRIPTION:
 * Sets color register @regnum.
 *
 * CALLED FROM:
 * xboxfb_setcolreg()
 */
static void riva_wclut(RIVA_HW_INST *chip,
		       unsigned char regnum, unsigned char red,
		       unsigned char green, unsigned char blue)
{
	VGA_WR08(chip->PDIO, 0x3c8, regnum);
	VGA_WR08(chip->PDIO, 0x3c9, red);
	VGA_WR08(chip->PDIO, 0x3c9, green);
	VGA_WR08(chip->PDIO, 0x3c9, blue);
}

/**
 * riva_rclut - read fromCLUT register
 * @chip: pointer to RIVA_HW_INST object
 * @regnum: register number
 * @red: red component
 * @green: green component
 * @blue: blue component
 *
 * DESCRIPTION:
 * Reads red, green, and blue from color register @regnum.
 *
 * CALLED FROM:
 * xboxfb_setcolreg()
 */
static void riva_rclut(RIVA_HW_INST *chip,
		       unsigned char regnum, unsigned char *red,
		       unsigned char *green, unsigned char *blue)
{
	
	VGA_WR08(chip->PDIO, 0x3c8, regnum);
	*red = VGA_RD08(chip->PDIO, 0x3c9);
	*green = VGA_RD08(chip->PDIO, 0x3c9);
	*blue = VGA_RD08(chip->PDIO, 0x3c9);
}

/**
 * riva_save_state - saves current chip state
 * @par: pointer to riva_par object containing info for current riva board
 * @regs: pointer to riva_regs object
 *
 * DESCRIPTION:
 * Saves current chip state to @regs.
 *
 * CALLED FROM:
 * xboxfb_probe()
 */
/* from GGI */
static void riva_save_state(struct riva_par *par, struct riva_regs *regs)
{
	int i;

	par->riva.LockUnlock(&par->riva, 0);

	par->riva.UnloadStateExt(&par->riva, &regs->ext);

	regs->misc_output = MISCin(par);

	for (i = 0; i < NUM_CRT_REGS; i++)
		regs->crtc[i] = CRTCin(par, i);

	for (i = 0; i < NUM_ATC_REGS; i++)
		regs->attr[i] = ATTRin(par, i);

	for (i = 0; i < NUM_GRC_REGS; i++)
		regs->gra[i] = GRAin(par, i);

	for (i = 0; i < NUM_SEQ_REGS; i++)
		regs->seq[i] = SEQin(par, i);
}

/**
 * riva_load_state - loads current chip state
 * @par: pointer to riva_par object containing info for current riva board
 * @regs: pointer to riva_regs object
 *
 * DESCRIPTION:
 * Loads chip state from @regs.
 *
 * CALLED FROM:
 * riva_load_video_mode()
 * xboxfb_probe()
 * xboxfb_remove()
 */
/* from GGI */
static void riva_load_state(struct riva_par *par, struct riva_regs *regs)
{
	RIVA_HW_STATE *state = &regs->ext;
	int i;

	CRTCout(par, 0x11, 0x00);

	par->riva.LockUnlock(&par->riva, 0);

	par->riva.LoadStateExt(&par->riva, state);

	par->riva.PGRAPH[0x00000820/4] = par->riva_fb_start;
	par->riva.PGRAPH[0x00000824/4] = par->riva_fb_start;
	par->riva.PGRAPH[0x00000828/4] = par->riva_fb_start;
	par->riva.PGRAPH[0x0000082c/4] = par->riva_fb_start;

	par->riva.PGRAPH[0x00000684/4] = par->riva.RamAmountKBytes * 1024 - 1;
	par->riva.PGRAPH[0x00000688/4] = par->riva.RamAmountKBytes * 1024 - 1;
	par->riva.PGRAPH[0x0000068c/4] = par->riva.RamAmountKBytes * 1024 - 1;
	par->riva.PGRAPH[0x00000690/4] = par->riva.RamAmountKBytes * 1024 - 1;
	par->riva.PRAMDAC[0x00000848/4] = 0x10100111;
	par->riva.PRAMDAC[0x00000880/4] = 0;
	par->riva.PRAMDAC[0x000008a0/4] = 0;
	par->riva.PMC[0x00008908/4] = par->riva.RamAmountKBytes * 1024 - 1;
	par->riva.PMC[0x0000890c/4] = par->riva.RamAmountKBytes * 1024 - 1;
	
        /* It has been decided to leave the GPU in RGB mode always, and handle
	* the scaling in the encoder, if necessary. This sidesteps the 2.6
	* kernel cursor issue seen with YUV output */
	if(par->video_encoder == ENCODER_XCALIBUR) {
		par->riva.PRAMDAC[0x00000630/4] = 2; // switch GPU to YCrCb
		/* YUV values: */
		par->riva.PRAMDAC[0x0000084c/4] = 0x00801080;
		par->riva.PRAMDAC[0x000008c4/4] = 0x40801080;
	} else {
		par->riva.PRAMDAC[0x00000630/4] = 0;
		/* These two need to be 0 on RGB to fix the maroon
		* borders issue */
		par->riva.PRAMDAC[0x0000084c/4] = 0;
		par->riva.PRAMDAC[0x000008c4/4] = 0;
	}
	
	MISCout(par, regs->misc_output);

	for (i = 0; i < NUM_CRT_REGS; i++) {
		switch (i) {
		case 0x0c:
		case 0x0d:
		case 0x19:
		case 0x20 ... 0x40:
			break;
		default:
			CRTCout(par, i, regs->crtc[i]);
		}
	}

	for (i = 0; i < NUM_ATC_REGS; i++)
		ATTRout(par, i, regs->attr[i]);

	for (i = 0; i < NUM_GRC_REGS; i++)
		GRAout(par, i, regs->gra[i]);

	for (i = 0; i < NUM_SEQ_REGS; i++)
		SEQout(par, i, regs->seq[i]);
	tv_save_mode(regs->encoder_regs);
}

static inline unsigned long xbox_memory_size(void) {
	/* make a guess on the xbox memory size. There are just
	two possibilities */
	if ((num_physpages << PAGE_SHIFT) > 64*1024*1024) {
		return 128*1024*1024;
	} else {
		return 64*1024*1024;
	}
}

static inline unsigned long available_framebuffer_memory(void) {
	return xbox_memory_size() - (num_physpages << PAGE_SHIFT);
}

/**
 * riva_load_video_mode - calculate timings
 * @info: pointer to fb_info object containing info for current riva board
 *
 * DESCRIPTION:
 * Calculate some timings and then send em off to riva_load_state().
 *
 * CALLED FROM:
 * xboxfb_set_par()
 */
static void riva_load_video_mode(struct fb_info *info)
{
	int bpp, width, height, hDisplaySize, hStart, hTotal, vStart, vTotal;
	int crtc_hDisplay, crtc_hStart, crtc_hEnd, crtc_hTotal;
	int crtc_vDisplay, crtc_vStart, crtc_vEnd, crtc_vTotal, dotClock;
	int crtc_hBlankStart, crtc_hBlankEnd, crtc_vBlankStart, crtc_vBlankEnd;
	struct riva_par *par = (struct riva_par *) info->par;
	struct riva_regs newmode;
	int encoder_ok = 0;	
	xbox_hdtv_mode hdtv_mode;
	xbox_video_mode encoder_mode;
	
	/* time to calculate */
	xboxfb_blank(1, info);

	bpp = info->var.bits_per_pixel;
	if (bpp == 16 && info->var.green.length == 5)
		bpp = 15;
	width = info->var.xres_virtual;
	height = info->var.yres_virtual;
	hDisplaySize = info->var.xres;
	hStart = hDisplaySize + info->var.right_margin;
	hTotal = hDisplaySize + info->var.right_margin +
		  info->var.hsync_len + info->var.left_margin;
	vStart = info->var.yres + info->var.lower_margin;
	vTotal = info->var.yres + info->var.lower_margin +
		 info->var.vsync_len + info->var.upper_margin;
	
	if (par->video_encoder==ENCODER_XCALIBUR) {
		/* This info should be in the xcalibur.c file, but
		* the HDTV api doesn't allow for it right now. */
		hTotal = 779;
		vTotal = 524;
	}
	
	crtc_hDisplay = (hDisplaySize / 8) - 1;
	crtc_hStart = (hTotal - 32) / 8;
	/* crtc_hStart = hStart / 8 - 1; */
	crtc_hEnd = crtc_hStart + 1;
	/* crtc_hEnd = (hStart + info->var.hsync_len) / 8 - 1; */
	crtc_hTotal = hTotal / 8 - 5;
	crtc_hBlankStart = crtc_hDisplay;
	crtc_hBlankEnd = crtc_hTotal + 4;
	
	crtc_vDisplay = info->var.yres - 1;
	/* crtc_vStart = vStart - 1; */
	crtc_vStart = vStart - 1;
	/* crtc_vEnd = vStart + info->var.vsync_len - 1; */
	crtc_vEnd = crtc_vStart + 2;
	crtc_vTotal = vTotal + 2;
	crtc_vDisplay = info->var.yres - 1;
	crtc_vBlankStart = crtc_vDisplay;
	crtc_vBlankEnd = crtc_vTotal + 1;
	
	dotClock = 1000000000 / info->var.pixclock;

	memcpy(&newmode, &reg_template, sizeof(struct riva_regs));

	if ((info->var.vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED)
		vTotal |= 1;

	if (par->FlatPanel) {
		crtc_vStart = crtc_vTotal - 3;
		crtc_vEnd = crtc_vTotal - 2;
		crtc_vBlankStart = crtc_vStart;
		crtc_hStart = crtc_hTotal - 3;
		crtc_hEnd = crtc_hTotal - 2;
		crtc_hBlankEnd = crtc_hTotal + 4;
	}

	newmode.ext.bpp = bpp;
	newmode.ext.fb_start = par->riva_fb_start;

	switch (par->av_type) {
		case AV_HDTV:
			if (info->var.yres > 800) {
				hdtv_mode = HDTV_1080i;
				crtc_vStart = vStart + 31;
				crtc_vEnd = crtc_vStart + 2;
			}
			else if (info->var.yres > 600) {
				hdtv_mode = HDTV_720p;
			}
			else hdtv_mode = HDTV_480p;

			switch (par->video_encoder) {
				case ENCODER_CONEXANT:
					encoder_ok = conexant_calc_hdtv_mode(hdtv_mode, dotClock, &(newmode.encoder_regs));
					break;
				case ENCODER_FOCUS:
					encoder_ok = focus_calc_hdtv_mode(hdtv_mode, dotClock, &(newmode.encoder_regs));
					break;
				case ENCODER_XCALIBUR:
					encoder_ok = xcalibur_calc_hdtv_mode(hdtv_mode, dotClock, &(newmode.encoder_regs));
					break;
				default:
					printk("Error - unknown encoder type detected\n");
			}
			newmode.ext.vend = info->var.yres - 1;
			newmode.ext.vtotal = vTotal;
			newmode.ext.vcrtc = info->var.yres - 1;
			newmode.ext.vsyncstart = vStart;
			newmode.ext.vsyncend = vStart + 3;
			newmode.ext.vvalidstart = 0;
			newmode.ext.vvalidend = info->var.yres - 1;
			newmode.ext.hend = info->var.xres - 1;
			newmode.ext.htotal = hTotal;
			newmode.ext.hcrtc = info->var.xres - 1;
			newmode.ext.hsyncstart = hStart;
			newmode.ext.hsyncend = hStart + 32;
			newmode.ext.hvalidstart = 0;
			newmode.ext.hvalidend = info->var.xres - 1;
			break;
	
		case AV_VGA:
		case AV_VGA_SOG:
			switch (par->video_encoder) {
				case ENCODER_CONEXANT:
					encoder_ok = conexant_calc_vga_mode(par->av_type, dotClock, &(newmode.encoder_regs));
					break;
				case ENCODER_FOCUS:
					//No vga functions as yet - so set up for 480p otherwise we dont boot at all. 
					encoder_ok = focus_calc_hdtv_mode(HDTV_480p, dotClock, &(newmode.encoder_regs));
					break;
				case ENCODER_XCALIBUR:
					//No vga functions as yet - so set up for 480p otherwise we dont boot at all. 
					encoder_ok = xcalibur_calc_hdtv_mode(HDTV_480p, dotClock, &(newmode.encoder_regs));
					break;
				default:
					printk("Error - unknown encoder type detected\n");
			}
			newmode.ext.vend = info->var.yres - 1;
			newmode.ext.vtotal = vTotal;
			newmode.ext.vcrtc = info->var.yres - 1;
			newmode.ext.vsyncstart = vStart;
			newmode.ext.vsyncend = vStart + 3;
			newmode.ext.vvalidstart = 0;
			newmode.ext.vvalidend = info->var.yres - 1;
			newmode.ext.hend = info->var.xres - 1;
			newmode.ext.htotal = hTotal;
			newmode.ext.hcrtc = info->var.xres - 1;
			newmode.ext.hsyncstart = hStart;
			newmode.ext.hsyncend = hStart + 32;
			newmode.ext.hvalidstart = 0;
			newmode.ext.hvalidend = info->var.xres - 1;
			break;
	
		default:	
			/* Normal composite */
			encoder_mode.xres = info->var.xres;
			encoder_mode.yres = info->var.yres;
			encoder_mode.tv_encoding = par->tv_encoding;
			encoder_mode.bpp = bpp;
			encoder_mode.hoc = par->hoc;
			encoder_mode.voc = par->voc;
			encoder_mode.av_type = par->av_type;
	
			switch (par->video_encoder) {
				case ENCODER_CONEXANT:
					encoder_ok = conexant_calc_mode(&encoder_mode, &newmode);
					break;
				case ENCODER_FOCUS:
					encoder_ok = focus_calc_mode(&encoder_mode, &newmode);
					break;
				case ENCODER_XCALIBUR:
					encoder_ok = xcalibur_calc_mode(&encoder_mode, &newmode);
					break;
				default:
					printk("Error - unknown encoder type detected\n");
			}
	
			crtc_hDisplay = (newmode.ext.crtchdispend / 8) - 1;
			crtc_hStart = (newmode.ext.htotal - 32) / 8;
			crtc_hEnd = crtc_hStart + 1;
			crtc_hTotal = (newmode.ext.htotal) / 8 - 5;
			crtc_hBlankStart = crtc_hDisplay;
			crtc_hBlankEnd = (newmode.ext.htotal) / 8 - 1;
			
			crtc_vDisplay = info->var.yres - 1;
			crtc_vStart = newmode.ext.crtcvstart;
			crtc_vEnd = newmode.ext.crtcvstart + 3;
			crtc_vTotal = newmode.ext.crtcvtotal;
			crtc_vBlankStart = crtc_vDisplay;
			crtc_vBlankEnd = crtc_vTotal + 1;
	}

	if (encoder_ok) {
		newmode.crtc[0x0] = Set8Bits (crtc_hTotal); 
		newmode.crtc[0x1] = Set8Bits (crtc_hDisplay);
		newmode.crtc[0x2] = Set8Bits (crtc_hBlankStart);
		newmode.crtc[0x3] = SetBitField (crtc_hBlankEnd, 4: 0, 4:0) | SetBit (7);
		newmode.crtc[0x4] = Set8Bits (crtc_hStart);
		newmode.crtc[0x5] = SetBitField (crtc_hBlankEnd, 5: 5, 7:7)
			| SetBitField (crtc_hEnd, 4: 0, 4:0);
		newmode.crtc[0x6] = SetBitField (crtc_vTotal, 7: 0, 7:0);
		newmode.crtc[0x7] = SetBitField (crtc_vTotal, 8: 8, 0:0)
			| SetBitField (crtc_vDisplay, 8: 8, 1:1)
			| SetBitField (crtc_vStart, 8: 8, 2:2)
			| SetBitField (crtc_vBlankStart, 8: 8, 3:3)
			| SetBit (4)
			| SetBitField (crtc_vTotal, 9: 9, 5:5)
			| SetBitField (crtc_vDisplay, 9: 9, 6:6)
			| SetBitField (crtc_vStart, 9: 9, 7:7);
		newmode.crtc[0x9] = SetBitField (crtc_vBlankStart, 9: 9, 5:5)
			| SetBit (6);
		newmode.crtc[0x10] = Set8Bits (crtc_vStart);
		newmode.crtc[0x11] = SetBitField (crtc_vEnd, 3: 0, 3:0)
			| SetBit (5);
		newmode.crtc[0x12] = Set8Bits (crtc_vDisplay);
		newmode.crtc[0x13] = (width / 8) * ((bpp + 1) / 8);
		newmode.crtc[0x15] = Set8Bits (crtc_vBlankStart);
		newmode.crtc[0x16] = Set8Bits (crtc_vBlankEnd);
	
		newmode.ext.screen = SetBitField(crtc_hBlankEnd,6:6,4:4)
			| SetBitField(crtc_vBlankStart,10:10,3:3)
			| SetBitField(crtc_vStart,10:10,2:2)
			| SetBitField(crtc_vDisplay,10:10,1:1)
			| SetBitField(crtc_vTotal,10:10,0:0);
		newmode.ext.horiz  = SetBitField(crtc_hTotal,8:8,0:0) 
			| SetBitField(crtc_hDisplay,8:8,1:1)
			| SetBitField(crtc_hBlankStart,8:8,2:2)
			| SetBitField(crtc_hStart,8:8,3:3);
		newmode.ext.extra  = SetBitField(crtc_vTotal,11:11,0:0)
			| SetBitField(crtc_vDisplay,11:11,2:2)
			| SetBitField(crtc_vStart,11:11,4:4)
			| SetBitField(crtc_vBlankStart,11:11,6:6); 
	
		if ((info->var.vmode & FB_VMODE_MASK) == FB_VMODE_INTERLACED) {
			int tmp = (crtc_hTotal >> 1) & ~1;
			newmode.ext.interlace = Set8Bits(tmp);
			newmode.ext.horiz |= SetBitField(tmp, 8:8,4:4);
		} else 
			newmode.ext.interlace = 0xff; /* interlace off */
	
		if (par->riva.Architecture >= NV_ARCH_10)
			/* use 2 KByte at top of framebuffer */
			par->riva.CURSOR = (U032 *)(info->screen_base + info->fix.smem_len - 2 * 1024);
	
		if (info->var.sync & FB_SYNC_HOR_HIGH_ACT)
			newmode.misc_output &= ~0x40;
		else
			newmode.misc_output |= 0x40;
		if (info->var.sync & FB_SYNC_VERT_HIGH_ACT)
			newmode.misc_output &= ~0x80;
		else
			newmode.misc_output |= 0x80;	
	
		par->riva.CalcStateExt(&par->riva, &newmode.ext, bpp, width,
					hDisplaySize, height, dotClock);
	
		newmode.ext.scale = par->riva.PRAMDAC[0x00000848/4] & 0xfff000ff;
		if (par->FlatPanel == 1) {
			newmode.ext.pixel |= (1 << 7);
			newmode.ext.scale |= (1 << 8);
		}
		if (par->SecondCRTC) {
			newmode.ext.head  = par->riva.PCRTC0[0x00000860/4] & ~0x00001000;
			newmode.ext.head2 = par->riva.PCRTC0[0x00002860/4] | 0x00001000;
			newmode.ext.crtcOwner = 3;
			newmode.ext.pllsel |= 0x20000800;
			newmode.ext.vpll2 = newmode.ext.vpll;
		} else if (par->riva.twoHeads) {
			newmode.ext.head  =  par->riva.PCRTC0[0x00000860/4] | 0x00001000;
			newmode.ext.head2 =  par->riva.PCRTC0[0x00002860/4] & ~0x00001000;
			newmode.ext.crtcOwner = 0;
			newmode.ext.vpll2 = par->riva.PRAMDAC0[0x00000520/4];
		}
		if (par->FlatPanel == 1) {
			newmode.ext.pixel |= (1 << 7);
			newmode.ext.scale |= (1 << 8);
		}
		newmode.ext.cursorConfig = 0x02000100;
		par->current_state = newmode;
		riva_load_state(par, &par->current_state);
		tv_load_mode(newmode.encoder_regs);
		par->riva.LockUnlock(&par->riva, 0); /* important for HW cursor */
	}
	else {
		printk("Error: Unable to set encoder resolution %dx%d\n",info->var.xres, info->var.yres);
	}
	
	xboxfb_blank(0, info);
}

static void riva_update_var(struct fb_var_screeninfo *var, struct fb_videomode *modedb)
{
	NVTRACE_ENTER();
	var->xres = var->xres_virtual = modedb->xres;
	var->yres = modedb->yres;
        if (var->yres_virtual < var->yres)
	    var->yres_virtual = var->yres;
        var->xoffset = var->yoffset = 0;
        var->pixclock = modedb->pixclock;
        var->left_margin = modedb->left_margin;
        var->right_margin = modedb->right_margin;
        var->upper_margin = modedb->upper_margin;
        var->lower_margin = modedb->lower_margin;
        var->hsync_len = modedb->hsync_len;
        var->vsync_len = modedb->vsync_len;
        var->sync = modedb->sync;
        var->vmode = modedb->vmode;
	NVTRACE_LEAVE();
}

/**
 * xboxfb_do_maximize - 
 * @info: pointer to fb_info object containing info for current riva board
 * @var:
 * @nom:
 * @den:
 *
 * DESCRIPTION:
 * .
 *
 * RETURNS:
 * -EINVAL on failure, 0 on success
 * 
 *
 * CALLED FROM:
 * xboxfb_check_var()
 */
static int xboxfb_do_maximize(struct fb_info *info,
			      struct fb_var_screeninfo *var,
			      int nom, int den)
{
	static struct {
		int xres, yres;
	} modes[] = {
		{1600, 1280},
		{1280, 1024},
		{1024, 768},
		{800, 600},
		{640, 480},
		{-1, -1}
	};
	int i;

	NVTRACE_ENTER();
	/* use highest possible virtual resolution */
	if (var->xres_virtual == -1 && var->yres_virtual == -1) {
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_WARNING PFX
		       "using maximum available virtual resolution\n");
#endif
		for (i = 0; modes[i].xres != -1; i++) {
			if (modes[i].xres * nom / den * modes[i].yres <
			    info->fix.smem_len)
				break;
		}
		if (modes[i].xres == -1) {
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_ERR PFX
			       "could not find a virtual resolution that fits into video memory!!\n");
#endif
			NVTRACE("EXIT - EINVAL error\n");
			return -EINVAL;
		}
		var->xres_virtual = modes[i].xres;
		var->yres_virtual = modes[i].yres;

#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_INFO PFX
		       "virtual resolution set to maximum of %dx%d\n",
		       var->xres_virtual, var->yres_virtual);
#endif
	} else if (var->xres_virtual == -1) {
		var->xres_virtual = (info->fix.smem_len * den /
			(nom * var->yres_virtual)) & ~15;
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_WARNING PFX
		       "setting virtual X resolution to %d\n", var->xres_virtual);
#endif
	} else if (var->yres_virtual == -1) {
		var->xres_virtual = (var->xres_virtual + 15) & ~15;
		var->yres_virtual = info->fix.smem_len * den /
			(nom * var->xres_virtual);
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_WARNING PFX
		       "setting virtual Y resolution to %d\n", var->yres_virtual);
#endif
	} else {
		var->xres_virtual = (var->xres_virtual + 15) & ~15;
		if (var->xres_virtual * nom / den * var->yres_virtual > info->fix.smem_len) {
#ifdef CONFIG_FB_RIVA_DEBUG
			printk(KERN_ERR PFX
			       "mode %dx%dx%d rejected...resolution too high to fit into video memory!\n",
			       var->xres, var->yres, var->bits_per_pixel);
#endif
			NVTRACE("EXIT - EINVAL error\n");
			return -EINVAL;
		}
	}
	
	if (var->xres_virtual * nom / den >= 8192) {
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_WARNING PFX
		       "virtual X resolution (%d) is too high, lowering to %d\n",
		       var->xres_virtual, 8192 * den / nom - 16);
#endif
		var->xres_virtual = 8192 * den / nom - 16;
	}
	
	if (var->xres_virtual < var->xres) {
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_ERR PFX
		       "virtual X resolution (%d) is smaller than real\n", var->xres_virtual);
#endif
		return -EINVAL;
	}

	if (var->yres_virtual < var->yres) {
#ifdef CONFIG_FB_RIVA_DEBUG
		printk(KERN_ERR PFX
		       "virtual Y resolution (%d) is smaller than real\n", var->yres_virtual);
#endif
		return -EINVAL;
	}
	if (var->yres_virtual > 0x7fff/nom)
		var->yres_virtual = 0x7fff/nom;
	if (var->xres_virtual > 0x7fff/nom)
		var->xres_virtual = 0x7fff/nom;
	NVTRACE_LEAVE();
	return 0;
}

static void
riva_set_pattern(struct riva_par *par, int clr0, int clr1, int pat0, int pat1)
{
	RIVA_FIFO_FREE(par->riva, Patt, 4);
	NV_WR32(&par->riva.Patt->Color0, 0, clr0);
	NV_WR32(&par->riva.Patt->Color1, 0, clr1);
	NV_WR32(par->riva.Patt->Monochrome, 0, pat0);
	NV_WR32(par->riva.Patt->Monochrome, 4, pat1);
}

/* acceleration routines */
inline void wait_for_idle(struct riva_par *par)
{
	while (par->riva.Busy(&par->riva));
}

/*
 * Set ROP.  Translate X rop into ROP3.  Internal routine.
 */
static void
riva_set_rop_solid(struct riva_par *par, int rop)
{
	riva_set_pattern(par, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
        RIVA_FIFO_FREE(par->riva, Rop, 1);
        NV_WR32(&par->riva.Rop->Rop3, 0, rop);

}

void riva_setup_accel(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;

	RIVA_FIFO_FREE(par->riva, Clip, 2);
	NV_WR32(&par->riva.Clip->TopLeft, 0, 0x0);
	NV_WR32(&par->riva.Clip->WidthHeight, 0,
		(info->var.xres_virtual & 0xffff) |
		(info->var.yres_virtual << 16));
	riva_set_rop_solid(par, 0xcc);
	wait_for_idle(par);
}

/**
 * riva_get_cmap_len - query current color map length
 * @var: standard kernel fb changeable data
 *
 * DESCRIPTION:
 * Get current color map length.
 *
 * RETURNS:
 * Length of color map
 *
 * CALLED FROM:
 * xboxfb_setcolreg()
 */
static int riva_get_cmap_len(const struct fb_var_screeninfo *var)
{
	int rc = 256;		/* reasonable default */

	switch (var->green.length) {
	case 8:
		rc = 256;	/* 256 entries (2^8), 8 bpp and RGB8888 */
		break;
	case 5:
		rc = 32;	/* 32 entries (2^5), 16 bpp, RGB555 */
		break;
	case 6:
		rc = 64;	/* 64 entries (2^6), 16 bpp, RGB565 */
		break;		
	default:
		/* should not occur */
		break;
	}
	return rc;
}

/* ------------------------------------------------------------------------- *
 *
 * Backlight operations
 *
 * ------------------------------------------------------------------------- */

#ifdef CONFIG_PMAC_BACKLIGHT
static int riva_set_backlight_enable(int on, int level, void *data)
{
	struct riva_par *par = (struct riva_par *)data;
	U032 tmp_pcrt, tmp_pmc;

	tmp_pmc = par->riva.PMC[0x10F0/4] & 0x0000FFFF;
	tmp_pcrt = par->riva.PCRTC0[0x081C/4] & 0xFFFFFFFC;
	if(on && (level > BACKLIGHT_OFF)) {
		tmp_pcrt |= 0x1;
		tmp_pmc |= (1 << 31); // backlight bit
		tmp_pmc |= riva_backlight_levels[level-1] << 16; // level
	}
	par->riva.PCRTC0[0x081C/4] = tmp_pcrt;
	par->riva.PMC[0x10F0/4] = tmp_pmc;
	return 0;
}

static int riva_set_backlight_level(int level, void *data)
{
	return riva_set_backlight_enable(1, level, data);
}
#endif /* CONFIG_PMAC_BACKLIGHT */

/* ------------------------------------------------------------------------- *
 *
 * framebuffer operations
 *
 * ------------------------------------------------------------------------- */

static int xboxfb_open(struct fb_info *info, int user)
{
	struct riva_par *par = (struct riva_par *) info->par;
	int cnt = atomic_read(&par->ref_count);

	if (!cnt) {
		memset(&par->state, 0, sizeof(struct vgastate));
		par->state.flags = VGA_SAVE_MODE  | VGA_SAVE_FONTS;
		/* save the DAC for Riva128 */
		if (par->riva.Architecture == NV_ARCH_03)
			par->state.flags |= VGA_SAVE_CMAP;
		save_vga(&par->state);

		RivaGetConfig(&par->riva, par->Chipset);
		par->riva.CURSOR = (U032*)(info->screen_base + info->fix.smem_len);
		par->riva.PCRTC[0x00000800/4] = par->riva_fb_start;
		par->riva.PGRAPH[0x00000820/4] = par->riva_fb_start;
		par->riva.PGRAPH[0x00000824/4] = par->riva_fb_start;
		par->riva.PGRAPH[0x00000828/4] = par->riva_fb_start;
		par->riva.PGRAPH[0x0000082c/4] = par->riva_fb_start;
		
		par->riva.PGRAPH[0x00000684/4] = par->riva.RamAmountKBytes * 1024 - 1;
		par->riva.PGRAPH[0x00000688/4] = par->riva.RamAmountKBytes * 1024 - 1;
		par->riva.PGRAPH[0x0000068c/4] = par->riva.RamAmountKBytes * 1024 - 1;
		par->riva.PGRAPH[0x00000690/4] = par->riva.RamAmountKBytes * 1024 - 1;
		par->riva.PMC[0x00008908/4] = par->riva.RamAmountKBytes * 1024 - 1;
		par->riva.PMC[0x0000890c/4] = par->riva.RamAmountKBytes * 1024 - 1;
		
		/* vgaHWunlock() + riva unlock (0x7F) */		
		CRTCout(par, 0x11, 0xFF);
		par->riva.LockUnlock(&par->riva, 0);
	
		riva_save_state(par, &par->initial_state);
	}
	atomic_inc(&par->ref_count);
	return 0;
}

static int xboxfb_release(struct fb_info *info, int user)
{
	struct riva_par *par = (struct riva_par *) info->par;
	int cnt = atomic_read(&par->ref_count);

	if (!cnt)
		return -EINVAL;
	if (cnt == 1) {
		par->riva.LockUnlock(&par->riva, 0);
		par->riva.LoadStateExt(&par->riva, &par->initial_state.ext);
		riva_load_state(par, &par->initial_state);
#ifdef CONFIG_X86
		restore_vga(&par->state);
#endif
		par->riva.LockUnlock(&par->riva, 1);
	}
	atomic_dec(&par->ref_count);
	return 0;
}

static int xboxfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct fb_videomode *mode;
	struct riva_par *par = (struct riva_par *) info->par;
	int nom, den;		/* translating from pixels->bytes */
	int mode_valid = 0;
	
	NVTRACE_ENTER();
	switch (var->bits_per_pixel) {
	case 1 ... 8:
		var->red.offset = var->green.offset = var->blue.offset = 0;
		var->red.length = var->green.length = var->blue.length = 8;
		var->bits_per_pixel = 8;
		nom = den = 1;
		break;
	case 9 ... 15:
		var->green.length = 5;
		/* fall through */
	case 16:
		var->bits_per_pixel = 16;
		/* The Riva128 supports RGB555 only */
		if (par->riva.Architecture == NV_ARCH_03)
			var->green.length = 5;
		if (var->green.length == 5) {
			/* 0rrrrrgg gggbbbbb */
			var->red.offset = 10;
			var->green.offset = 5;
			var->blue.offset = 0;
			var->red.length = 5;
			var->green.length = 5;
			var->blue.length = 5;
		} else {
			/* rrrrrggg gggbbbbb */
			var->red.offset = 11;
			var->green.offset = 5;
			var->blue.offset = 0;
			var->red.length = 5;
			var->green.length = 6;
			var->blue.length = 5;
		}
		nom = 2;
		den = 1;
		break;
	case 17 ... 32:
		var->red.length = var->green.length = var->blue.length = 8;
		var->bits_per_pixel = 32;
		var->red.offset = 16;
		var->green.offset = 8;
		var->blue.offset = 0;
		nom = 4;
		den = 1;
		break;
	default:
		printk(KERN_ERR PFX
		       "mode %dx%dx%d rejected...color depth not supported.\n",
		       var->xres, var->yres, var->bits_per_pixel);
		NVTRACE("EXIT, returning -EINVAL\n");
		return -EINVAL;
	}

	if (!strictmode) {
		if (!info->monspecs.vfmax || !info->monspecs.hfmax ||
		    !info->monspecs.dclkmax || !fb_validate_mode(var, info))
			mode_valid = 1;
	}

	/* calculate modeline if supported by monitor */
	if (!mode_valid && info->monspecs.gtf) {
		if (!fb_get_mode(FB_MAXTIMINGS, 0, var, info))
			mode_valid = 1;
	}

	if (!mode_valid) {
		mode = fb_find_best_mode(var, &info->modelist);
		if (mode) {
			riva_update_var(var, mode);
			mode_valid = 1;
		}
	}

	if (!mode_valid && info->monspecs.modedb_len)
		return -EINVAL;

	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->yres_virtual <= var->yres)
		var->yres_virtual = -1;
	if (xboxfb_do_maximize(info, var, nom, den) < 0)
		return -EINVAL;

	if (var->xoffset < 0)
		var->xoffset = 0;
	if (var->yoffset < 0)
		var->yoffset = 0;

	/* truncate xoffset and yoffset to maximum if too high */
	if (var->xoffset > var->xres_virtual - var->xres)
		var->xoffset = var->xres_virtual - var->xres - 1;

	if (var->yoffset > var->yres_virtual - var->yres)
		var->yoffset = var->yres_virtual - var->yres - 1;

	var->red.msb_right = 
	    var->green.msb_right =
	    var->blue.msb_right =
	    var->transp.offset = var->transp.length = var->transp.msb_right = 0;
	NVTRACE_LEAVE();
	return 0;
}

static int xboxfb_set_par(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;

	NVTRACE_ENTER();
	/* vgaHWunlock() + riva unlock (0x7F) */
	CRTCout(par, 0x11, 0xFF);
	par->riva.LockUnlock(&par->riva, 0);
	riva_load_video_mode(info);
	if(!(info->flags & FBINFO_HWACCEL_DISABLED))
		riva_setup_accel(info);
	
	par->cursor_reset = 1;
	info->fix.line_length = (info->var.xres_virtual * (info->var.bits_per_pixel >> 3));
	info->fix.visual = (info->var.bits_per_pixel == 8) ?
				FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_DIRECTCOLOR;

	if (info->flags & FBINFO_HWACCEL_DISABLED)
		info->pixmap.scan_align = 1;
	else
		info->pixmap.scan_align = 4;
	NVTRACE_LEAVE();
	return 0;
}

/**
 * xboxfb_pan_display
 * @var: standard kernel fb changeable data
 * @con: TODO
 * @info: pointer to fb_info object containing info for current riva board
 *
 * DESCRIPTION:
 * Pan (or wrap, depending on the `vmode' field) the display using the
 * `xoffset' and `yoffset' fields of the `var' structure.
 * If the values don't fit, return -EINVAL.
 *
 * This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
 */
static int xboxfb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *)info->par;
	unsigned int base;

	NVTRACE_ENTER();
	if (var->xoffset > (var->xres_virtual - var->xres))
		return -EINVAL;
	if (var->yoffset > (var->yres_virtual - var->yres))
		return -EINVAL;

	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset < 0
		    || var->yoffset >= info->var.yres_virtual
		    || var->xoffset) return -EINVAL;
	} else {
		if (var->xoffset + info->var.xres > info->var.xres_virtual ||
		    var->yoffset + info->var.yres > info->var.yres_virtual)
			return -EINVAL;
	}

	base = var->yoffset * info->fix.line_length + var->xoffset;
	base += par->riva_fb_start;

	par->riva.SetStartAddress(&par->riva, base);

	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;

	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;
	NVTRACE_LEAVE();
	return 0;
}

static int xboxfb_blank(int blank, struct fb_info *info)
{
	struct riva_par *par= (struct riva_par *)info->par;
	unsigned char tmp, vesa;

	tmp = SEQin(par, 0x01) & ~0x20;	/* screen on/off */
	vesa = CRTCin(par, 0x1a) & ~0xc0;	/* sync on/off */

	NVTRACE_ENTER();

	if (blank)
		tmp |= 0x20;

	switch (blank) {
	case FB_BLANK_UNBLANK:
	case FB_BLANK_NORMAL:
		break;
	case FB_BLANK_VSYNC_SUSPEND:
		vesa |= 0x80;
		break;
	case FB_BLANK_HSYNC_SUSPEND:
		vesa |= 0x40;
		break;
	case FB_BLANK_POWERDOWN:
		vesa |= 0xc0;
		break;
	}

	SEQout(par, 0x01, tmp);
	CRTCout(par, 0x1a, vesa);

#ifdef CONFIG_PMAC_BACKLIGHT
	if ( par->FlatPanel && _machine == _MACH_Pmac) {
		set_backlight_enable(!blank);
	}
#endif

	NVTRACE_LEAVE();

	return 0;
}

/**
 * xboxfb_setcolreg
 * @regno: register index
 * @red: red component
 * @green: green component
 * @blue: blue component
 * @transp: transparency
 * @info: pointer to fb_info object containing info for current riva board
 *
 * DESCRIPTION:
 * Set a single color register. The values supplied have a 16 bit
 * magnitude.
 *
 * RETURNS:
 * Return != 0 for invalid regno.
 *
 * CALLED FROM:
 * fbcmap.c:fb_set_cmap()
 */
static int xboxfb_setcolreg(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp,
			  struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *)info->par;
	RIVA_HW_INST *chip = &par->riva;
	int i;

	if (regno >= riva_get_cmap_len(&info->var))
			return -EINVAL;

	if (info->var.grayscale) {
		/* gray = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

	if (regno < 16 && info->fix.visual == FB_VISUAL_DIRECTCOLOR) {
		((u32 *) info->pseudo_palette)[regno] =
			(regno << info->var.red.offset) |
			(regno << info->var.green.offset) |
			(regno << info->var.blue.offset);
		/*
		 * The Riva128 2D engine requires color information in
		 * TrueColor format even if framebuffer is in DirectColor
		 */
		if (par->riva.Architecture == NV_ARCH_03) {
			switch (info->var.bits_per_pixel) {
			case 16:
				par->palette[regno] = ((red & 0xf800) >> 1) |
					((green & 0xf800) >> 6) |
					((blue & 0xf800) >> 11);
				break;
			case 32:
				par->palette[regno] = ((red & 0xff00) << 8) |
					((green & 0xff00)) |
					((blue & 0xff00) >> 8);
				break;
			}
		}
	}

	switch (info->var.bits_per_pixel) {
	case 8:
		/* "transparent" stuff is completely ignored. */
		riva_wclut(chip, regno, red >> 8, green >> 8, blue >> 8);
		break;
	case 16:
		if (info->var.green.length == 5) {
			for (i = 0; i < 8; i++) {
				riva_wclut(chip, regno*8+i, red >> 8,
					   green >> 8, blue >> 8);
			}
		} else {
			u8 r, g, b;

			if (regno < 32) {
				for (i = 0; i < 8; i++) {
					riva_wclut(chip, regno*8+i,
						   red >> 8, green >> 8,
						   blue >> 8);
				}
			}
			riva_rclut(chip, regno*4, &r, &g, &b);
			for (i = 0; i < 4; i++)
				riva_wclut(chip, regno*4+i, r,
					   green >> 8, b);
		}
		break;
	case 32:
		riva_wclut(chip, regno, red >> 8, green >> 8, blue >> 8);
		break;
	default:
		/* do nothing */
		break;
	}
	return 0;
}

/**
 * xboxfb_fillrect - hardware accelerated color fill function
 * @info: pointer to fb_info structure
 * @rect: pointer to fb_fillrect structure
 *
 * DESCRIPTION:
 * This function fills up a region of framebuffer memory with a solid
 * color with a choice of two different ROP's, copy or invert.
 *
 * CALLED FROM:
 * framebuffer hook
 */
static void xboxfb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct riva_par *par = (struct riva_par *) info->par;
	u_int color, rop = 0;

	if ((info->flags & FBINFO_HWACCEL_DISABLED)) {
		cfb_fillrect(info, rect);
		return;
	}

	if (info->var.bits_per_pixel == 8)
		color = rect->color;
	else {
		if (par->riva.Architecture != NV_ARCH_03)
			color = ((u32 *)info->pseudo_palette)[rect->color];
		else
			color = par->palette[rect->color];
	}

	switch (rect->rop) {
	case ROP_XOR:
		rop = 0x66;
		break;
	case ROP_COPY:
	default:
		rop = 0xCC;
		break;
	}

	riva_set_rop_solid(par, rop);

	RIVA_FIFO_FREE(par->riva, Bitmap, 1);
	NV_WR32(&par->riva.Bitmap->Color1A, 0, color);

	RIVA_FIFO_FREE(par->riva, Bitmap, 2);
	NV_WR32(&par->riva.Bitmap->UnclippedRectangle[0].TopLeft, 0,
		(rect->dx << 16) | rect->dy);
	mb();
	NV_WR32(&par->riva.Bitmap->UnclippedRectangle[0].WidthHeight, 0,
		(rect->width << 16) | rect->height);
	mb();
	riva_set_rop_solid(par, 0xcc);

}

/**
 * xboxfb_copyarea - hardware accelerated blit function
 * @info: pointer to fb_info structure
 * @region: pointer to fb_copyarea structure
 *
 * DESCRIPTION:
 * This copies an area of pixels from one location to another
 *
 * CALLED FROM:
 * framebuffer hook
 */
static void xboxfb_copyarea(struct fb_info *info, const struct fb_copyarea *region)
{
	struct riva_par *par = (struct riva_par *) info->par;

	if ((info->flags & FBINFO_HWACCEL_DISABLED)) {
		cfb_copyarea(info, region);
		return;
	}

	RIVA_FIFO_FREE(par->riva, Blt, 3);
	NV_WR32(&par->riva.Blt->TopLeftSrc, 0,
		(region->sy << 16) | region->sx);
	NV_WR32(&par->riva.Blt->TopLeftDst, 0,
		(region->dy << 16) | region->dx);
	mb();
	NV_WR32(&par->riva.Blt->WidthHeight, 0,
		(region->height << 16) | region->width);
	mb();
}

static inline void convert_bgcolor_16(u32 *col)
{
	*col = ((*col & 0x0000F800) << 8)
		| ((*col & 0x00007E0) << 5)
		| ((*col & 0x0000001F) << 3)
		|	   0xFF000000;
	mb();
}

/**
 * xboxfb_imageblit: hardware accelerated color expand function
 * @info: pointer to fb_info structure
 * @image: pointer to fb_image structure
 *
 * DESCRIPTION:
 * If the source is a monochrome bitmap, the function fills up a a region
 * of framebuffer memory with pixels whose color is determined by the bit
 * setting of the bitmap, 1 - foreground, 0 - background.
 *
 * If the source is not a monochrome bitmap, color expansion is not done.
 * In this case, it is channeled to a software function.
 *
 * CALLED FROM:
 * framebuffer hook
 */
static void xboxfb_imageblit(struct fb_info *info, 
			     const struct fb_image *image)
{
	struct riva_par *par = (struct riva_par *) info->par;
	u32 fgx = 0, bgx = 0, width, tmp;
	u8 *cdat = (u8 *) image->data;
	volatile u32 __iomem *d;
	int i, size;

	if ((info->flags & FBINFO_HWACCEL_DISABLED) || image->depth != 1) {
		cfb_imageblit(info, image);
		return;
	}

	switch (info->var.bits_per_pixel) {
	case 8:
		fgx = image->fg_color;
		bgx = image->bg_color;
		break;
	case 16:
	case 32:
		if (par->riva.Architecture != NV_ARCH_03) {
			fgx = ((u32 *)info->pseudo_palette)[image->fg_color];
			bgx = ((u32 *)info->pseudo_palette)[image->bg_color];
		} else {
			fgx = par->palette[image->fg_color];
			bgx = par->palette[image->bg_color];
		}
		if (info->var.green.length == 6)
			convert_bgcolor_16(&bgx);	
		break;
	}

	RIVA_FIFO_FREE(par->riva, Bitmap, 7);
	NV_WR32(&par->riva.Bitmap->ClipE.TopLeft, 0,
		(image->dy << 16) | (image->dx & 0xFFFF));
	NV_WR32(&par->riva.Bitmap->ClipE.BottomRight, 0,
		(((image->dy + image->height) << 16) |
		 ((image->dx + image->width) & 0xffff)));
	NV_WR32(&par->riva.Bitmap->Color0E, 0, bgx);
	NV_WR32(&par->riva.Bitmap->Color1E, 0, fgx);
	NV_WR32(&par->riva.Bitmap->WidthHeightInE, 0,
		(image->height << 16) | ((image->width + 31) & ~31));
	NV_WR32(&par->riva.Bitmap->WidthHeightOutE, 0,
		(image->height << 16) | ((image->width + 31) & ~31));
	NV_WR32(&par->riva.Bitmap->PointE, 0,
		(image->dy << 16) | (image->dx & 0xFFFF));

	d = &par->riva.Bitmap->MonochromeData01E;

	width = (image->width + 31)/32;
	size = width * image->height;
	while (size >= 16) {
		RIVA_FIFO_FREE(par->riva, Bitmap, 16);
		for (i = 0; i < 16; i++) {
			tmp = *((u32 *)cdat);
			cdat = (u8 *)((u32 *)cdat + 1);
			reverse_order(&tmp);
			NV_WR32(d, i*4, tmp);
		}
		size -= 16;
	}
	if (size) {
		RIVA_FIFO_FREE(par->riva, Bitmap, size);
		for (i = 0; i < size; i++) {
			tmp = *((u32 *) cdat);
			cdat = (u8 *)((u32 *)cdat + 1);
			reverse_order(&tmp);
			NV_WR32(d, i*4, tmp);
		}
	}
}

/**
 * xboxfb_cursor - hardware cursor function
 * @info: pointer to info structure
 * @cursor: pointer to fbcursor structure
 *
 * DESCRIPTION:
 * A cursor function that supports displaying a cursor image via hardware.
 * Within the kernel, copy and invert rops are supported.  If exported
 * to user space, only the copy rop will be supported.
 *
 * CALLED FROM
 * framebuffer hook
 */
static int xboxfb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	struct riva_par *par = (struct riva_par *) info->par;
	u8 data[MAX_CURS * MAX_CURS/8];
	u16 fg, bg;
	int i, set = cursor->set;

	if (cursor->image.width > MAX_CURS ||
	    cursor->image.height > MAX_CURS)
		return soft_cursor(info, cursor);

	par->riva.ShowHideCursor(&par->riva, 0);

	if (par->cursor_reset) {
		set = FB_CUR_SETALL;
		par->cursor_reset = 0;
	}

	if (set & FB_CUR_SETSIZE)
		memset_io(par->riva.CURSOR, 0, MAX_CURS * MAX_CURS * 2);

	if (set & FB_CUR_SETPOS) {
		u32 xx, yy, temp;

		yy = cursor->image.dy - info->var.yoffset;
		xx = cursor->image.dx - info->var.xoffset;
		temp = xx & 0xFFFF;
		temp |= yy << 16;

		NV_WR32(par->riva.PRAMDAC, 0x0000300, temp);
	}


	if (set & (FB_CUR_SETSHAPE | FB_CUR_SETCMAP | FB_CUR_SETIMAGE)) {
		u32 bg_idx = cursor->image.bg_color;
		u32 fg_idx = cursor->image.fg_color;
		u32 s_pitch = (cursor->image.width+7) >> 3;
		u32 d_pitch = MAX_CURS/8;
		u8 *dat = (u8 *) cursor->image.data;
		u8 *msk = (u8 *) cursor->mask;
		u8 *src;
		
		src = kmalloc(s_pitch * cursor->image.height, GFP_ATOMIC);

		if (src) {
			switch (cursor->rop) {
			case ROP_XOR:
				for (i = 0; i < s_pitch * cursor->image.height;
				     i++)
					src[i] = dat[i] ^ msk[i];
				break;
			case ROP_COPY:
			default:
				for (i = 0; i < s_pitch * cursor->image.height;
				     i++)
					src[i] = dat[i] & msk[i];
				break;
			}

			fb_pad_aligned_buffer(data, d_pitch, src, s_pitch,
					       cursor->image.height);

			bg = ((info->cmap.red[bg_idx] & 0xf8) << 7) |
				((info->cmap.green[bg_idx] & 0xf8) << 2) |
				((info->cmap.blue[bg_idx] & 0xf8) >> 3) |
				1 << 15;

			fg = ((info->cmap.red[fg_idx] & 0xf8) << 7) |
				((info->cmap.green[fg_idx] & 0xf8) << 2) |
				((info->cmap.blue[fg_idx] & 0xf8) >> 3) |
				1 << 15;

			par->riva.LockUnlock(&par->riva, 0);

			xboxfb_load_cursor_image(par, data, bg, fg,
						 cursor->image.width,
						 cursor->image.height);
			kfree(src);
		}
	}

	if (cursor->enable)
		par->riva.ShowHideCursor(&par->riva, 1);

	return 0;
}

static int xboxfb_sync(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *)info->par;

	wait_for_idle(par);
	return 0;
}

static int xboxfb_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
                        unsigned long arg, struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;
	
	xbox_overscan overscan;
	xboxfb_config config;
	xbox_tv_encoding encoding;
	int ret = 0;

	switch (cmd) {
	case FBIO_XBOX_SET_OVERSCAN:
		if(!copy_from_user(&overscan, (xbox_overscan*)arg, sizeof(overscan))) {
			par->hoc = overscan.hoc;
			par->voc = overscan.voc;
			riva_load_video_mode (info);
			if(!(info->flags & FBINFO_HWACCEL_DISABLED)) {
				riva_setup_accel(info);
			}
		}
		else {
			ret = -EFAULT;
		}
	break;
	case FBIO_XBOX_GET_OVERSCAN:
		overscan.hoc = par->hoc;
		overscan.voc = par->voc;
		if (copy_to_user((xbox_overscan*)arg, &overscan, sizeof(overscan))) {
			ret = -EFAULT;
		}
	break;
	case FBIO_XBOX_GET_CONFIG:
		config.av_type = par->av_type;
		config.encoder_type = par->video_encoder;
		if (copy_to_user((xboxfb_config*)arg, &config, sizeof(config))) {
			ret = -EFAULT;
		}
	break;
	case FBIO_XBOX_GET_TV_ENCODING:
		encoding = par->tv_encoding;
		if (copy_to_user((xbox_tv_encoding*)arg, &encoding, sizeof(encoding))) {
			ret = -EFAULT;
		}
	break;
	case FBIO_XBOX_SET_TV_ENCODING:
		if(!copy_from_user(&encoding, (xbox_tv_encoding*)arg, sizeof(encoding))) {
			par->tv_encoding = encoding;
			riva_load_video_mode (info);
			if(!(info->flags & FBINFO_HWACCEL_DISABLED)) {
				riva_setup_accel(info);
			}
		}
		else {
			ret = -EFAULT;
		}
	break;
	default:
		ret = -EINVAL;
	}
	return ret;
}


/* ------------------------------------------------------------------------- *
 *
 * initialization helper functions
 *
 * ------------------------------------------------------------------------- */

/* kernel interface */
static struct fb_ops riva_fb_ops = {
	.owner 		= THIS_MODULE,
	.fb_open	= xboxfb_open,
	.fb_release	= xboxfb_release,
	.fb_check_var 	= xboxfb_check_var,
	.fb_set_par 	= xboxfb_set_par,
	.fb_setcolreg 	= xboxfb_setcolreg,
	.fb_pan_display	= xboxfb_pan_display,
	.fb_blank 	= xboxfb_blank,
	.fb_fillrect 	= xboxfb_fillrect,
	.fb_copyarea 	= xboxfb_copyarea,
	.fb_imageblit 	= xboxfb_imageblit,
	.fb_cursor	= xboxfb_cursor,
	.fb_sync 	= xboxfb_sync,
	.fb_ioctl	= xboxfb_ioctl,
};

static int __devinit riva_set_fbinfo(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;
	unsigned int cmap_len;

	info->flags = FBINFO_FLAG_DEFAULT;
	info->var = xboxfb_default_var;
	info->fix = xboxfb_fix;
	info->fbops = &riva_fb_ops;
	info->pseudo_palette = pseudo_palette;

#ifndef MODULE
	if (mode_option)
	{
		if (!strncmp(mode_option, "480p", 4)) {
			info->var = xboxfb_mode_480p;
		}
		else if (!strncmp(mode_option, "720p", 4)) {
			info->var = xboxfb_mode_720p;
		}
		else {
			fb_find_mode(&info->var, info, mode_option,
				NULL, 0, NULL, 8);
		}
	}
#endif
	if (par->use_default_var)
		/* We will use the modified default var */
		info->var = xboxfb_default_var;

	cmap_len = riva_get_cmap_len(&info->var);
	fb_alloc_cmap(&info->cmap, cmap_len, 0);	

	info->pixmap.size = 64 * 1024;
	info->pixmap.buf_align = 4;
	info->pixmap.scan_align = 4;
	info->pixmap.flags = FB_PIXMAP_SYSTEM;
	return 0;
}

#ifdef CONFIG_PPC_OF
static int riva_get_EDID_OF(struct fb_info *info, struct pci_dev *pd)
{
	struct riva_par *par = (struct riva_par *) info->par;
	struct device_node *dp;
	unsigned char *pedid = NULL;

	dp = pci_device_to_OF_node(pd);
	pedid = (unsigned char *)get_property(dp, "EDID,B", 0);

	if (pedid) {
		par->EDID = pedid;
		return 1;
	} else
		return 0;
}
#endif /* CONFIG_PPC_OF */

static int riva_dfp_parse_EDID(struct riva_par *par)
{
	unsigned char *block = par->EDID;

	if (!block)
		return 0;

	/* jump to detailed timing block section */
	block += 54;

	par->clock = (block[0] + (block[1] << 8));
	par->panel_xres = (block[2] + ((block[4] & 0xf0) << 4));
	par->hblank = (block[3] + ((block[4] & 0x0f) << 8));
	par->panel_yres = (block[5] + ((block[7] & 0xf0) << 4));
	par->vblank = (block[6] + ((block[7] & 0x0f) << 8));
	par->hOver_plus = (block[8] + ((block[11] & 0xc0) << 2));
	par->hSync_width = (block[9] + ((block[11] & 0x30) << 4));
	par->vOver_plus = ((block[10] >> 4) + ((block[11] & 0x0c) << 2));
	par->vSync_width = ((block[10] & 0x0f) + ((block[11] & 0x03) << 4));
	par->interlaced = ((block[17] & 0x80) >> 7);
	par->synct = ((block[17] & 0x18) >> 3);
	par->misc = ((block[17] & 0x06) >> 1);
	par->hAct_high = par->vAct_high = 0;
	if (par->synct == 3) {
		if (par->misc & 2)
			par->hAct_high = 1;
		if (par->misc & 1)
			par->vAct_high = 1;
	}

	printk(KERN_INFO PFX
			"detected DFP panel size from EDID: %dx%d\n", 
			par->panel_xres, par->panel_yres);
	par->got_dfpinfo = 1;
	return 1;
}

static void riva_update_default_var(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &xboxfb_default_var;
	struct riva_par *par = (struct riva_par *) info->par;

        var->xres = par->panel_xres;
        var->yres = par->panel_yres;
        var->xres_virtual = par->panel_xres;
        var->yres_virtual = par->panel_yres;
        var->xoffset = var->yoffset = 0;
        var->bits_per_pixel = 8;
        var->pixclock = 100000000 / par->clock;
        var->left_margin = (par->hblank - par->hOver_plus - par->hSync_width);
        var->right_margin = par->hOver_plus;
        var->upper_margin = (par->vblank - par->vOver_plus - par->vSync_width);
        var->lower_margin = par->vOver_plus;
        var->hsync_len = par->hSync_width;
        var->vsync_len = par->vSync_width;
        var->sync = 0;

        if (par->synct == 3) {
                if (par->hAct_high)
                        var->sync |= FB_SYNC_HOR_HIGH_ACT;
                if (par->vAct_high)
                        var->sync |= FB_SYNC_VERT_HIGH_ACT;
        }
 
        var->vmode = 0;
        if (par->interlaced)
                var->vmode |= FB_VMODE_INTERLACED;

	var->accel_flags |= FB_ACCELF_TEXT;
        
        par->use_default_var = 1;
}


static void riva_get_EDID(struct fb_info *info, struct pci_dev *pdev)
{
#ifdef CONFIG_PPC_OF
	if (!riva_get_EDID_OF(info, pdev))
		printk("xboxfb: could not retrieve EDID from OF\n");
#else
	/* XXX use other methods later */
#endif
}


static void riva_get_dfpinfo(struct fb_info *info)
{
	struct riva_par *par = (struct riva_par *) info->par;

	if (riva_dfp_parse_EDID(par))
		riva_update_default_var(info);

	/* if user specified flatpanel, we respect that */
	if (par->got_dfpinfo == 1)
		par->FlatPanel = 1;
}

/* ------------------------------------------------------------------------- *
 *
 * PCI bus
 *
 * ------------------------------------------------------------------------- */

static int __devinit xboxfb_probe(struct pci_dev *pd,
			     	const struct pci_device_id *ent)
{
	struct riva_chip_info *rci = &riva_chip_info[ent->driver_data];
	struct riva_par *default_par;
	struct fb_info *info;
	unsigned long fb_start;
	unsigned long fb_size;

	assert(pd != NULL);
	assert(rci != NULL);

	info = kmalloc(sizeof(struct fb_info), GFP_KERNEL);
	if (!info)
		goto err_out;

	default_par = kmalloc(sizeof(struct riva_par), GFP_KERNEL);
	if (!default_par)
		goto err_out_kfree;

	memset(info, 0, sizeof(struct fb_info));
	memset(default_par, 0, sizeof(struct riva_par));

	info->pixmap.addr = kmalloc(64 * 1024, GFP_KERNEL);
	if (info->pixmap.addr == NULL)
		goto err_out_kfree1;
	memset(info->pixmap.addr, 0, 64 * 1024);

	strcat(xboxfb_fix.id, rci->name);
	default_par->riva.Architecture = rci->arch_rev;

	default_par->Chipset = (pd->vendor << 16) | pd->device;
	printk(KERN_INFO PFX "nVidia device/chipset %X\n",default_par->Chipset);
	
	default_par->FlatPanel = flatpanel;
	if (flatpanel == 1)
		printk(KERN_INFO PFX "flatpanel support enabled\n");
	default_par->forceCRTC = forceCRTC;
	
	xboxfb_fix.mmio_len = pci_resource_len(pd, 0);

	{
		/* enable IO and mem if not already done */
		unsigned short cmd;

		pci_read_config_word(pd, PCI_COMMAND, &cmd);
		cmd |= (PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
		pci_write_config_word(pd, PCI_COMMAND, cmd);
	}
	
	xboxfb_fix.mmio_start = pci_resource_start(pd, 0);
	xboxfb_fix.smem_start = pci_resource_start(pd, 1);
	
        if (xbox_memory_size() == 64*1024*1024) printk(KERN_INFO PFX "Detected 64MB of system RAM\n");
	else printk(KERN_INFO PFX "Detected 128MB of system RAM\n");
	
	fb_size = available_framebuffer_memory();
	fb_start = xbox_memory_size() - fb_size;
	printk(KERN_INFO PFX "Using %dM framebuffer memory\n", (int)(fb_size/(1024*1024)));
	default_par->riva_fb_start = fb_start;
	xboxfb_fix.smem_start += fb_start;
	xboxfb_fix.smem_len = fb_size; 
	tv_init();
	if (tv_encoding == TV_ENC_INVALID) {
		tv_encoding = get_tv_encoding();
		printk(KERN_INFO PFX "Setting TV mode from EEPROM (%s)\n", tvEncodingNames[tv_encoding]);
	}
	default_par->tv_encoding = tv_encoding;
	default_par->video_encoder = tv_get_video_encoder();
	switch(default_par->video_encoder) {
		case ENCODER_CONEXANT:
			printk(KERN_INFO PFX "detected conexant encoder\n");
			break;
		case ENCODER_FOCUS:
			printk(KERN_INFO PFX "detected focus encoder\n");
			break;
		case ENCODER_XCALIBUR:
			printk(KERN_INFO PFX "detected Xcalibur encoder\n");
			break;
		default: 
			printk(KERN_INFO PFX "detected unknown encoder\n");
	}
	
	
	if (av_type == AV_INVALID) {
		av_type = detect_av_type();
		printk(KERN_INFO PFX "Setting cable type from AVIP ID: %s\n", avTypeNames[av_type]);
	}
	default_par->av_type = av_type;
	if ((hoc < 0) || (hoc > 20)) {
		hoc = 10;
	}
	default_par->hoc = hoc / 100.0;
	if ((voc < 0) || (voc > 20)) {
		voc = 10;
	}
	default_par->voc = voc / 100.0;
	
	if (!request_mem_region(xboxfb_fix.mmio_start,
				xboxfb_fix.mmio_len, "xboxfb")) {
		printk(KERN_ERR PFX "cannot reserve MMIO region\n");
		goto err_out_kfree2;
	}

	default_par->ctrl_base = ioremap(xboxfb_fix.mmio_start,
					 xboxfb_fix.mmio_len);
	if (!default_par->ctrl_base) {
		printk(KERN_ERR PFX "cannot ioremap MMIO base\n");
		goto err_out_free_base0;
	}

	info->par = default_par;

	riva_get_EDID(info, pd);

	riva_get_dfpinfo(info);

	switch (default_par->riva.Architecture) {
	case NV_ARCH_03:
		/* Riva128's PRAMIN is in the "framebuffer" space
		 * Since these cards were never made with more than 8 megabytes
		 * we can safely allocate this separately.
		 */
		if (!request_mem_region(xboxfb_fix.smem_start + 0x00C00000,
					 0x00008000, "xboxfb")) {
			printk(KERN_ERR PFX "cannot reserve PRAMIN region\n");
			goto err_out_iounmap_ctrl;
		}
		default_par->riva.PRAMIN = ioremap(xboxfb_fix.smem_start + 0x00C00000, 0x00008000);
		if (!default_par->riva.PRAMIN) {
			printk(KERN_ERR PFX "cannot ioremap PRAMIN region\n");
			goto err_out_free_nv3_pramin;
		}
		xboxfb_fix.accel = FB_ACCEL_NV3;
		break;
	case NV_ARCH_04:
	case NV_ARCH_10:
	case NV_ARCH_20:
		default_par->riva.PCRTC0 = (unsigned *)(default_par->ctrl_base + 0x00600000);
		default_par->riva.PRAMIN = (unsigned *)(default_par->ctrl_base + 0x00710000);
		xboxfb_fix.accel = FB_ACCEL_NV4;
		break;
	}

	riva_common_setup(default_par);

	if (default_par->riva.Architecture == NV_ARCH_03) {
		default_par->riva.PCRTC = default_par->riva.PCRTC0 = default_par->riva.PGRAPH;
	}

	/* xboxfb_fix.smem_len = riva_get_memlen(default_par) * 1024; */
	default_par->dclk_max = riva_get_maxdclk(default_par) * 1000;

	if (!request_mem_region(xboxfb_fix.smem_start, xboxfb_fix.smem_len, "xboxfb")) {
		printk(KERN_ERR PFX "cannot reserve FB region\n");
		goto err_out_iounmap_nv3_pramin;
	}
	
	info->screen_base = ioremap(xboxfb_fix.smem_start, fb_size);
	if (!info->screen_base) {
		printk(KERN_ERR PFX "cannot ioremap FB base\n");
		goto err_out_free_base1;
	}

#ifdef CONFIG_MTRR
	if (!nomtrr) {
		default_par->mtrr.vram = mtrr_add(xboxfb_fix.smem_start, fb_size,
			MTRR_TYPE_WRCOMB, 1);
		if (default_par->mtrr.vram < 0) {
			printk(KERN_ERR PFX "unable to setup MTRR\n");
		} else {
			default_par->mtrr.vram_valid = 1;
			/* let there be speed */
			printk(KERN_INFO PFX "RIVA MTRR set to ON\n");
		}
	}
#endif /* CONFIG_MTRR */

	if (riva_set_fbinfo(info) < 0) {
		printk(KERN_ERR PFX "error setting initial video mode\n");
		goto err_out_iounmap_fb;
	}

	if (register_framebuffer(info) < 0) {
		printk(KERN_ERR PFX
			"error registering riva framebuffer\n");
		goto err_out_iounmap_fb;
	}

	pci_set_drvdata(pd, info);

	printk(KERN_INFO PFX
		"PCI nVidia NV%x framebuffer ver %s (%s, %ldMB @ 0x%lX)\n",
		default_par->riva.Architecture,
		XBOXFB_VERSION,
		info->fix.id,
		fb_size / (1024 * 1024),
		info->fix.smem_start);
	return 0;

err_out_iounmap_fb:
	iounmap(info->screen_base);
err_out_free_base1:
	release_mem_region(xboxfb_fix.smem_start, fb_size);
err_out_iounmap_nv3_pramin:
	if (default_par->riva.Architecture == NV_ARCH_03) 
		iounmap((caddr_t)default_par->riva.PRAMIN);
err_out_free_nv3_pramin:
	if (default_par->riva.Architecture == NV_ARCH_03)
		release_mem_region(xboxfb_fix.smem_start + 0x00C00000, 0x00008000);
err_out_iounmap_ctrl:
	iounmap(default_par->ctrl_base);
err_out_free_base0:
	release_mem_region(xboxfb_fix.mmio_start, xboxfb_fix.mmio_len);
err_out_kfree2:
	kfree(info->pixmap.addr);
err_out_kfree1:
	kfree(default_par);
err_out_kfree:
	kfree(info);
err_out:
	return -ENODEV;
}

static void __exit xboxfb_remove(struct pci_dev *pd)
{
	struct fb_info *info = pci_get_drvdata(pd);
	struct riva_par *par = (struct riva_par *) info->par;
	
	if (!info)
		return;

	unregister_framebuffer(info);
#ifdef CONFIG_MTRR
	if (par->mtrr.vram_valid)
		mtrr_del(par->mtrr.vram, info->fix.smem_start, info->fix.smem_len);
#endif /* CONFIG_MTRR */

	iounmap(par->ctrl_base);
	iounmap(info->screen_base);

	release_mem_region(info->fix.mmio_start,
			   info->fix.mmio_len);
	release_mem_region(info->fix.smem_start, info->fix.smem_len);

	if (par->riva.Architecture == NV_ARCH_03) {
		iounmap((caddr_t)par->riva.PRAMIN);
		release_mem_region(info->fix.smem_start + 0x00C00000, 0x00008000);
	}
	kfree(info->pixmap.addr);
	kfree(par);
	kfree(info);
	pci_set_drvdata(pd, NULL);
}

/* ------------------------------------------------------------------------- *
 *
 * initialization
 *
 * ------------------------------------------------------------------------- */

int __init xboxfb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!strncmp(this_opt, "forceCRTC", 9)) {
			char *p;
			
			p = this_opt + 9;
			if (!*p || !*(++p)) continue; 
			forceCRTC = *p - '0';
			if (forceCRTC < 0 || forceCRTC > 1) 
				forceCRTC = -1;
		} else if (!strncmp(this_opt, "flatpanel", 9)) {
			flatpanel = 1;
#ifdef CONFIG_MTRR
		} else if (!strncmp(this_opt, "nomtrr", 6)) {
			nomtrr = 1;
#endif
		} else if (!strncmp(this_opt, "strictmode", 10)) {
			strictmode = 1;
		} else if (!strncmp(this_opt, "noaccel", 7)) {
			noaccel = 1;
		} else if (!strncmp(this_opt, "tv=", 3)) {
				if(!strncmp(this_opt + 3, "PAL", 3)) {
						tv_encoding = TV_ENC_PALBDGHI;
				}
				else if(!strncmp(this_opt + 3, "NTSC", 4)) {
						tv_encoding = TV_ENC_NTSC;
				}
				else if(!strncmp(this_opt + 3, "VGA", 3)) {
						av_type = AV_VGA_SOG;
				}
		} else if (!strncmp(this_opt, "hoc=", 4)) {
				sscanf(this_opt+4, "%d", &hoc);
		} else if (!strncmp(this_opt, "voc=", 4)) {
				sscanf(this_opt+4, "%d", &voc);
		} else
			mode_option = this_opt;
	}
	return 0;
}

static struct pci_driver xboxfb_driver = {
	.name		= "xboxfb",
	.id_table	= xboxfb_pci_tbl,
	.probe		= xboxfb_probe,
	.remove		= __exit_p(xboxfb_remove),
};



/* ------------------------------------------------------------------------- *
 *
 * modularization
 *
 * ------------------------------------------------------------------------- */

int __devinit xboxfb_init(void)
{
        char *option = NULL;

	//Ignore error here, vesafb does!
	fb_get_options("xboxfb", &option);
//        if (fb_get_options("xboxfb", &option))
//		return -ENODEV;
	xboxfb_setup(option);
	
	return pci_register_driver(&xboxfb_driver);
}

module_init(xboxfb_init);

#ifdef MODULE
static void __exit xboxfb_exit(void)
{
	pci_unregister_driver(&xboxfb_driver);
}

module_exit(xboxfb_exit);

module_param(noaccel, bool, 0);
MODULE_PARM_DESC(noaccel, "bool: disable acceleration");
MODULE_PARM(flatpanel, "i");
MODULE_PARM_DESC(flatpanel, "Enables experimental flat panel support for some chipsets. (0 or 1=enabled) (default=0)");
MODULE_PARM(forceCRTC, "i");
MODULE_PARM_DESC(forceCRTC, "Forces usage of a particular CRTC in case autodetection fails. (0 or 1) (default=autodetect)");

#ifdef CONFIG_MTRR
MODULE_PARM(nomtrr, "i");
MODULE_PARM_DESC(nomtrr, "Disables MTRR support (0 or 1=disabled) (default=0)");
#endif
module_param(strictmode, bool, 0);
MODULE_PARM_DESC(strictmode, "Only use video modes from EDID");

MODULE_PARM(tv, "s");
MODULE_PARM_DESC(tv, "Specifies the TV encoding (\"PAL\", \"NTSC\" or \"VGA\").");
MODULE_PARM(hoc, "i");
MODULE_PARM_DESC(hoc, "Horizontal overscan compensation ratio, in % (0-20)");
MODULE_PARM(voc, "i");
MODULE_PARM_DESC(voc, "Vertical overscan compensation ratio, in % (0-20)");

#endif /* MODULE */

MODULE_AUTHOR("Oliver Schwartz");
MODULE_DESCRIPTION("Framebuffer driver for Xbox");
MODULE_LICENSE("GPL");
