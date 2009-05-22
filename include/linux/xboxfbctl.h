/*
 * linux/include/video/xboxfbctl.h
 * - Type definitions for ioctls of Xbox video driver
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

#ifndef xbofbctl_h
#define xbofbctl_h

typedef enum enumVideoStandards {
	TV_ENC_INVALID=-1,
	TV_ENC_NTSC=0,
	TV_ENC_NTSC60,
	TV_ENC_PALBDGHI,
	TV_ENC_PALN,
	TV_ENC_PALNC,
	TV_ENC_PALM,
	TV_ENC_PAL60
} xbox_tv_encoding;

typedef enum enumAvTypes {
	AV_INVALID=-1,
	AV_SCART_RGB,
	AV_SVIDEO,
	AV_VGA_SOG,
	AV_HDTV,
	AV_COMPOSITE,
	AV_VGA
} xbox_av_type;

typedef enum enumEncoderType {
	ENCODER_CONEXANT,
	ENCODER_FOCUS,
	ENCODER_XCALIBUR
} xbox_encoder_type;

typedef struct _xboxOverscan {
	double hoc;
	double voc;
} xbox_overscan;

typedef struct _xboxFbConfig {
	xbox_av_type av_type;
	xbox_encoder_type encoder_type;
} xboxfb_config;

#define FBIO_XBOX_GET_OVERSCAN  _IOR('x', 1, xbox_overscan)
/* in param: double  hoc (0.0-0.2), double voc (0.0 - 0.2) */
#define FBIO_XBOX_SET_OVERSCAN  _IOW('x', 2, xbox_overscan)

#define FBIO_XBOX_GET_TV_ENCODING  _IOR('x', 3, xbox_tv_encoding)
#define FBIO_XBOX_SET_TV_ENCODING  _IOW('x', 4, xbox_tv_encoding)

#define FBIO_XBOX_GET_CONFIG  _IOR('x', 5, xboxfb_config)

#endif
