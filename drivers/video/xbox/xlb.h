/*
 * linux/drivers/video/riva/xlb.c - Xbox driver for Xcalibur encoder
 *
 * Maintainer: David Pye (dmp) <dmp@davidmpye.dyndns.org>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Known bugs and issues:
 *
 * none
 */


#ifndef xlb_h_
#define xlb_h_

#include "encoder.h"
#include "xboxfb.h"

int xlb_calc_mode(xbox_video_mode * mode, struct riva_regs * riva_out );
int xlb_calc_hdtv_mode(xbox_hdtv_mode hdtv_mode, unsigned char pll_int, unsigned char * mode_out);
#endif
