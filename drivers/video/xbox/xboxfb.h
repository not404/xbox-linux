#ifndef __XBOXFB_H
#define __XBOXFB_H

#include <linux/config.h>
#include <linux/fb.h>
#include <video/vga.h>
#include "riva_hw.h"
#include <linux/xboxfbctl.h>

/* GGI compatibility macros */
#define NUM_SEQ_REGS		0x05
#define NUM_CRT_REGS		0x41
#define NUM_GRC_REGS		0x09
#define NUM_ATC_REGS		0x15

#define NUM_CONEXANT_REGS	0x69
#define NUM_FOCUS_REGS 	    0xff
#define MAX_ENCODER_REGS    0xff

/* holds the state of the VGA core and extended Riva hw state from riva_hw.c.
 * From KGI originally. */
struct riva_regs {
	u8 attr[NUM_ATC_REGS];
	u8 crtc[NUM_CRT_REGS];
	u8 gra[NUM_GRC_REGS];
	u8 seq[NUM_SEQ_REGS];
	u8 misc_output;
	RIVA_HW_STATE ext;
	void *encoder_regs;
};

struct riva_par {
	RIVA_HW_INST riva;	/* interface to riva_hw.c */
	u32 pseudo_palette[16]; /* default palette */
	u32 palette[16];        /* for Riva128 */
	u8 __iomem *ctrl_base;	/* virtual control register base addr */
	unsigned dclk_max;	/* max DCLK */

	struct riva_regs initial_state;	/* initial startup video mode */
	struct riva_regs current_state;
	struct vgastate state;
	atomic_t ref_count;
	u32 cursor_data[32 * 32/4];
	unsigned char *EDID;

	int panel_xres, panel_yres;
	int hOver_plus, hSync_width, hblank;
	int vOver_plus, vSync_width, vblank;
	int hAct_high, vAct_high, interlaced;
	int synct, misc, clock;

	int use_default_var;
	int got_dfpinfo;
	unsigned int Chipset;
	int forceCRTC;
	Bool SecondCRTC;
	int FlatPanel;
	struct pci_dev *pdev;
	int bus;
	int cursor_reset;
#ifdef CONFIG_MTRR
	struct { int vram; int vram_valid; } mtrr;
#endif
	unsigned riva_fb_start; /* start address of fb in riva address space */
	xbox_tv_encoding tv_encoding;
	xbox_av_type av_type;
	xbox_encoder_type video_encoder;
	double hoc;
	double voc;
};

void riva_common_setup(struct riva_par *);
unsigned long riva_get_memlen(struct riva_par *);
unsigned long riva_get_maxdclk(struct riva_par *);

#endif /* __XBOXFB_H */
