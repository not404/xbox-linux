/*
 * linux/drivers/video/riva/encoder.h - Xbox driver for encoder chip
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


#ifndef encoder_h
#define encoder_h

#include <linux/xboxfbctl.h>

typedef struct {
	double m_dHzBurstFrequency;
	double m_dSecBurstStart;
	double m_dSecBurstEnd;
	double m_dSecHsyncWidth;
	double m_dSecHsyncPeriod;
	double m_dSecActiveBegin;
	double m_dSecImageCentre;
	double m_dSecBlankBeginToHsync;
	unsigned int m_dwALO;
	double m_TotalLinesOut;
	double m_dSecHsyncToBlankEnd;
} conexant_video_parameter;

typedef struct _xbox_video_mode {
	int xres;
	int yres;
	int bpp;
	double hoc;
	double voc;
	xbox_av_type av_type;
	xbox_tv_encoding tv_encoding;
} xbox_video_mode;

typedef enum enumHdtvModes {
        HDTV_480p,
	HDTV_720p,
	HDTV_1080i
} xbox_hdtv_mode;

static const conexant_video_parameter vidstda[];

int tv_init(void);
void tv_exit(void);
xbox_encoder_type tv_get_video_encoder(void);

void tv_save_mode(unsigned char * mode_out);
void tv_load_mode(unsigned char * mode);
xbox_tv_encoding get_tv_encoding(void);
xbox_av_type detect_av_type(void);

#endif
