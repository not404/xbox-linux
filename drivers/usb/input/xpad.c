/*
 * Xbox input device driver for Linux - v0.1.5
 *
 * Copyright (c)  2002 - 2004  Marko Friedemann <mfr@bmx-chemnitz.de>
 *
 *	Contributors:
 *		Vojtech Pavlik <vojtech@suse.sz>,
 *		Oliver Schwartz <Oliver.Schwartz@gmx.de>,
 *		Thomas Pedley <gentoox@shallax.com>,
 *		Steven Toth <steve@toth.demon.co.uk>,
 *		Franz Lehner <franz@caos.at>,
 *		Ivan Hawkes <blackhawk@ivanhawkes.com>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * This driver is based on:
 *  - information from     http://euc.jp/periphs/xbox-controller.en.html
 *  - the iForce driver    drivers/char/joystick/iforce.c
 *  - the skeleton-driver  drivers/usb/usb-skeleton.c
 *
 * Thanks to:
 *  - ITO Takayuki for providing essential xpad information on his website
 *  - Vojtech Pavlik     - iforce driver / input subsystem
 *  - Greg Kroah-Hartman - usb-skeleton driver
 *
 * TODO:
 *  - fine tune axes
 *  - NEW: Test right thumb stick Y-axis to see if it needs flipping.
 *  - NEW: get rumble working correctly, fix all the bugs and support multiple
 *         simultaneous effects
 *  - NEW: split funtionality mouse/joustick into two source files
 *  - NEW: implement /proc interface (toggle mouse/rumble enable/disable, etc.)
 *  - NEW: implement user space daemon application that handles that interface
 *
 * History: moved to end of file
 */
 
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/usb.h>
#include <linux/usb_input.h>
#include <linux/version.h>
#include <linux/timer.h>
#include <asm/uaccess.h>

#include "xpad.h"

static struct xpad_device xpad_device[] = {
	/* please keep those ordered wrt. vendor/product ids
	  vendor, product, isMat, name                              */
	{ 0x044f, 0x0f07, 0, "Thrustmaster, Inc. Controller" },
	{ 0x045e, 0x0202, 0, "Microsoft Xbox Controller" },
	{ 0x045e, 0x0285, 0, "Microsoft Xbox Controller S" },
	{ 0x045e, 0x0287, 0, "Microsoft Xbox Controller S" },
	{ 0x045e, 0x0289, 0, "Microsoft Xbox Controller S" }, /* microsoft is stupid */
	{ 0x046d, 0xca84, 0, "Logitech Xbox Cordless Controller" },
	{ 0x046d, 0xca88, 0, "Logitech Compact Controller for Xbox" },
	{ 0x05fd, 0x1007, 0, "???Mad Catz Controller???" }, /* CHECKME: this seems strange */
	{ 0x05fd, 0x107a, 0, "InterAct PowerPad Pro" },
	{ 0x0738, 0x4516, 0, "Mad Catz Control Pad" },
	{ 0x0738, 0x4522, 0, "Mad Catz LumiCON" },
	{ 0x0738, 0x4526, 0, "Mad Catz Control Pad Pro" },
	{ 0x0738, 0x4536, 0, "Mad Catz MicroCON" },
	{ 0x0738, 0x4540, 1, "Mad Catz Beat Pad" },
	{ 0x0738, 0x4556, 0, "Mad Catz Lynx Wireless Controller" },
	{ 0x0738, 0x6040, 1, "Mad Catz Beat Pad Pro" },
	{ 0x0c12, 0x8802, 0, "Zeroplus Xbox Controller" },
	{ 0x0c12, 0x8809, 0, "Level Six Xbox DDR Dancepad" },
	{ 0x0c12, 0x8810, 0, "Zeroplus Xbox Controller" },
	{ 0x0c12, 0x9902, 0, "HAMA VibraX - *FAULTY HARDWARE*" }, /* these are broken */
	{ 0x0e4c, 0x1097, 0, "Radica Gamester Controller" },	
	{ 0x0e4c, 0x2390, 0, "Radica Games Jtech Controller" },	
	{ 0x0e6f, 0x0003, 0, "Logic3 Freebird wireless Controller" },
	{ 0x0e6f, 0x0005, 0, "Eclipse wireless Controller" },
	{ 0x0e6f, 0x0006, 0, "Edge wireless Controller" },
	{ 0x0f30, 0x0202, 0, "Joytech Advanced Controller" },
	{ 0x102c, 0xff0c, 0, "Joytech Wireless Advanced Controller" },
	{ 0x12ab, 0x8809, 1, "Xbox DDR dancepad" },
	{ 0xffff, 0xffff, 0, "Chinese-made Xbox Controller" }, /* WTF are device IDs for? */
	{ 0x0000, 0x0000, 0, "nothing detected - FAIL" }
};

static signed short xpad_btn[] = {
	BTN_A, BTN_B, BTN_C, BTN_X, BTN_Y, BTN_Z,	/* analogue buttons */
	BTN_START, BTN_BACK, BTN_THUMBL, BTN_THUMBR,	/* start/back/sticks */
	BTN_0, BTN_1, BTN_2, BTN_3,			/* d-pad as buttons */
	-1						/* terminating entry */
};

static signed short xpad_mat_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y, 	/* A, B, X, Y */
	BTN_START, BTN_BACK, 		/* start/back */
	BTN_0, BTN_1, BTN_2, BTN_3,	/* directions */
	-1				/* terminating entry */
};

static signed short xpad_abs[] = {
	ABS_X, ABS_Y,		/* left stick */
	ABS_RX, ABS_RY,		/* right stick */
	ABS_Z, ABS_RZ,		/* triggers left/right */
	ABS_HAT0X, ABS_HAT0Y,	/* digital pad */
	ABS_HAT1X, ABS_HAT1Y,	/* analogue buttons A + B */
	ABS_HAT2X, ABS_HAT2Y,	/* analogue buttons C + X */
	ABS_HAT3X, ABS_HAT3Y,	/* analogue buttons Y + Z */
	-1			/* terminating entry */
};

static struct usb_device_id xpad_table [] = {
	{ USB_INTERFACE_INFO('X', 'B', 0) },	/* Xbox USB-IF not approved class */
	{ USB_INTERFACE_INFO( 3 ,  0 , 0) },	/* for Joytech Advanced Controller */
	{ }
};

MODULE_DEVICE_TABLE(usb, xpad_table);

/**
 *	xpad_process_packet
 *
 *	Completes a request by converting the data into events
 *	for the input subsystem.
 *
 *	The report descriptor was taken from ITO Takayukis website:
 *	 http://euc.jp/periphs/xbox-controller.en.html
 */
static void xpad_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data, struct pt_regs *regs)
{
	struct input_dev *dev = &xpad->dev;

	input_regs(dev, regs);

	/* digital pad (button mode) bits (3 2 1 0) (right left down up) */
	input_report_key(dev, BTN_0, (data[2] & 0x01));
	input_report_key(dev, BTN_1, (data[2] & 0x08) >> 3);
	input_report_key(dev, BTN_2, (data[2] & 0x02) >> 1);
	input_report_key(dev, BTN_3, (data[2] & 0x04) >> 2);	

	/* start and back buttons */
	input_report_key(dev, BTN_START, (data[2] & 0x10) >> 4);
	input_report_key(dev, BTN_BACK, (data[2] & 0x20) >> 5);

	/* buttons A, B, X, Y digital mode */
	input_report_key(dev, BTN_A, data[4]);
	input_report_key(dev, BTN_B, data[5]);
	input_report_key(dev, BTN_X, data[6]);
	input_report_key(dev, BTN_Y, data[7]);

	if (xpad->isMat)
		return;

	/* left stick (Y axis needs to be flipped) */
	input_report_abs(dev, ABS_X, (__s16)(((__s16)data[13] << 8) | (__s16)data[12]));
	input_report_abs(dev, ABS_Y, ~(__s16)(((__s16)data[15] << 8) | data[14]));

	/* right stick */
	input_report_abs(dev, ABS_RX, (__s16)(((__s16)data[17] << 8) | (__s16)data[16]));
	input_report_abs(dev, ABS_RY, (__s16)(((__s16)data[19] << 8) | (__s16)data[18]));

	/* triggers left/right */
	input_report_abs(dev, ABS_Z, data[10]);
	input_report_abs(dev, ABS_RZ, data[11]);

	/* digital pad (analogue mode): bits (3 2 1 0) (right left down up) */
	input_report_abs(dev, ABS_HAT0X, !!(data[2] & 0x08) - !!(data[2] & 0x04));
	input_report_abs(dev, ABS_HAT0Y, !!(data[2] & 0x01) - !!(data[2] & 0x02));

	/* stick press left/right */
	input_report_key(dev, BTN_THUMBL, (data[2] & 0x40) >> 6);
	input_report_key(dev, BTN_THUMBR, data[2] >> 7);

	/* button A, B, X, Y analogue mode */
	input_report_abs(dev, ABS_HAT1X, data[4]);
	input_report_abs(dev, ABS_HAT1Y, data[5]);
	input_report_abs(dev, ABS_HAT2Y, data[6]);
	input_report_abs(dev, ABS_HAT3X, data[7]);

	/* button C (black) digital/analogue mode */
	input_report_key(dev, BTN_C, data[8]);
	input_report_abs(dev, ABS_HAT2X, data[8]);

	/* button Z (white) digital/analogue mode */
	input_report_key(dev, BTN_Z, data[9]);
	input_report_abs(dev, ABS_HAT3Y, data[9]);

	input_sync(dev);
}

/**
 *	xpad_irq_in
 *
 *	Completion handler for interrupt in transfers (user input).
 *	Just calls xpad_process_packet which does then emit input events.
 */
static void xpad_irq_in(struct urb *urb, struct pt_regs *regs)
{
	struct usb_xpad *xpad = urb->context;
	int retval;

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d",
		    __FUNCTION__, urb->status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d",
		    __FUNCTION__, urb->status);
		goto exit;
	}

	xpad_process_packet(xpad, 0, xpad->idata, regs);

exit:
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		err("%s - usb_submit_urb failed with result %d",
		    __FUNCTION__, retval);
}

/**
 *	xpad_open
 *
 *	Called when a an application opens the device.
 */
static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = dev->private;
	int status;

	info("opening device");

	xpad->irq_in->dev = xpad->udev;
	if ((status = usb_submit_urb(xpad->irq_in, GFP_KERNEL))) {
		err("open input urb failed: %d", status);
		return -EIO;
	}

	xpad_rumble_open(xpad);

	return 0;
}

/**
 *	xpad_close
 *
 *	Called when an application closes the device.
 */
static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = dev->private;

	info("closing device");
	usb_kill_urb(xpad->irq_in);
	xpad_rumble_close(xpad);
}

/**	xpad_init_input_device
 *
 *	setup the input device for the kernel
 */
static void xpad_init_input_device(struct usb_interface *intf, struct xpad_device device)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct usb_device *udev = interface_to_usbdev(intf);
	char path[64];
	int i;

	usb_to_input_id(udev, &xpad->dev.id);
	xpad->dev.dev = &intf->dev;
	xpad->dev.private = xpad;
	xpad->dev.name = device.name;
	xpad->dev.phys = xpad->phys;
	xpad->dev.open = xpad_open;
	xpad->dev.close = xpad_close;

	usb_make_path(udev, path, 64);
	snprintf(xpad->phys, 64, "%s/input0", path);

	/* this was meant to allow a user space tool on-the-fly configuration
	   of driver options (rumble on, etc...)
	   yet, Vojtech said this is better done using sysfs (linux 2.6)
	   plus, it needs a patch to the input subsystem */
/*	xpad->dev.ioctl = xpad_ioctl;*/

	if (xpad->isMat) {
		xpad->dev.evbit[0] = BIT(EV_KEY);
		for (i = 0; xpad_mat_btn[i] >= 0; ++i)
			set_bit(xpad_mat_btn[i], xpad->dev.keybit);
	} else {
		xpad->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);

		for (i = 0; xpad_btn[i] >= 0; ++i)
		set_bit(xpad_btn[i], xpad->dev.keybit);

		for (i = 0; xpad_abs[i] >= 0; ++i) {
			signed short t = xpad_abs[i];

			set_bit(t, xpad->dev.absbit);

			switch (t) {
			case ABS_X:
			case ABS_Y:
			case ABS_RX:
			case ABS_RY:	/* the two sticks */
				xpad->dev.absmax[t] =  32767;
				xpad->dev.absmin[t] = -32768;
				xpad->dev.absflat[t] = 12000;
				xpad->dev.absfuzz[t] = 16;
				break;
			case ABS_Z:	/* left trigger */
			case ABS_RZ:	/* right trigger */
			case ABS_HAT1X:	/* analogue button A */
			case ABS_HAT1Y:	/* analogue button B */
			case ABS_HAT2X:	/* analogue button C */
			case ABS_HAT2Y:	/* analogue button X */
			case ABS_HAT3X:	/* analogue button Y */
			case ABS_HAT3Y:	/* analogue button Z */
				xpad->dev.absmax[t] = 255;
				xpad->dev.absmin[t] = 0;
				break;
			case ABS_HAT0X:
			case ABS_HAT0Y:	/* the d-pad */
				xpad->dev.absmax[t] =  1;
				xpad->dev.absmin[t] = -1;
				break;
			}
		}

		if (xpad_rumble_probe(udev, xpad, ifnum) != 0)
			err("could not init rumble");
	}

	input_register_device(&xpad->dev);
	printk(KERN_INFO "input: %s on %s\n", xpad->dev.name, path);
}

/**
 *	xpad_probe
 *
 *	Called upon device detection to find a suitable driver.
 *	Must return NULL when no xpad is found, else setup everything.
 */
static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_xpad *xpad = NULL;
	struct usb_endpoint_descriptor *ep_irq_in;
	int i;
	int probedDevNum = -1;	/* this takes the index into the known devices
				   array for the recognized device */

	/* try to detect the device we are called for */
	for (i = 0; xpad_device[i].idVendor; ++i) {
		if ((udev->descriptor.idVendor == xpad_device[i].idVendor) &&
		    (udev->descriptor.idProduct == xpad_device[i].idProduct)) {
			probedDevNum = i;
			break;
		}
	}

	/* sanity check, did we recognize this device? if not, fail */
	if ((probedDevNum == -1) || (!xpad_device[probedDevNum].idVendor &&
				     !xpad_device[probedDevNum].idProduct))
		return -ENODEV;

	if ((xpad = kmalloc (sizeof(struct usb_xpad), GFP_KERNEL)) == NULL) {
		err("cannot allocate memory for new pad");
		return -ENOMEM;
	}
	memset(xpad, 0, sizeof(struct usb_xpad));

	xpad->idata = usb_buffer_alloc(udev, XPAD_PKT_LEN,
				       SLAB_ATOMIC, &xpad->idata_dma);

	if (!xpad->idata) {
		kfree(xpad);
		return -ENOMEM;
	}

	/* setup input interrupt pipe (button and axis state) */
	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
        if (!xpad->irq_in) {
		err("cannot allocate memory for new pad irq urb");
		usb_buffer_free(udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
                kfree(xpad);
                return -ENOMEM;
	}

	ep_irq_in = &intf->altsetting[0].endpoint[0].desc;

	xpad->udev = udev;
	xpad->isMat = xpad_device[probedDevNum].isMat;

	/* init input URB for USB INT transfer from device */
	usb_fill_int_urb(xpad->irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->idata, XPAD_PKT_LEN,
			 xpad_irq_in, xpad, ep_irq_in->bInterval);
	xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* we set this here so we can extract it in the two functions below */
	usb_set_intfdata(intf, xpad);

	xpad_init_input_device(intf, xpad_device[probedDevNum]);

	return 0;
}

/**
 *	xpad_disconnect
 *
 *	Called upon device disconnect to dispose of the structures and
 *	close the USB connections.
 */
static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (xpad) {
		info( "disconnecting device" );
		usb_kill_urb(xpad->irq_in);
		xpad_rumble_close(xpad);
		input_unregister_device(&xpad->dev);

		usb_free_urb(xpad->irq_in);

		usb_buffer_free(interface_to_usbdev(intf), XPAD_PKT_LEN,
				xpad->idata, xpad->idata_dma);

		xpad_rumble_disconnect(xpad);

		kfree(xpad);
	}
}

/******************* Linux driver framework specific stuff ************/

static struct usb_driver xpad_driver = {
	.owner		= THIS_MODULE,
	.name		= "xpad",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.id_table	= xpad_table,
};

/**
 * driver init entry point
 */
static int __init usb_xpad_init(void)
{
	int result = usb_register(&xpad_driver);
	if (result == 0)
		info(DRIVER_DESC " " DRIVER_VERSION);
	return result;
}

/**
 * driver exit entry point
 */
static void __exit usb_xpad_exit(void)
{
	usb_deregister(&xpad_driver);
}

module_init(usb_xpad_init);
module_exit(usb_xpad_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

/*
 *  driver history
 * ----------------
 *
 * 2005-03-15 - 0.1.5 : Mouse emulation removed.  Deadzones increased.
 *  - Flipped the Y axis of the left joystick (it was inverted, like on a 
 *    flight simulator).
 *
 * 2003-05-15 - 0.1.2 : ioctls, dynamic mouse/rumble activation, /proc fs
 *  - added some /proc files for informational purposes (readonly right now)
 *  - added init parameters for mouse/rumble activation upon detection
 *  - added dynamic changes to mouse events / rumble effect generation via
 *    ioctls - NOTE: this requires a currently unofficial joydev patch!
 *
 * 2003-04-29 - 0.1.1 : minor cleanups, some comments
 *  - fixed incorrect handling of unknown devices (please try ir dongle now)
 *  - fixed input URB length (the 256 bytes from 0.1.0 broke everything for the
 *    MS controller as well as my Interact device, set back to 32 (please
 *    REPORT problems BEFORE any further changes here, since those can be fatal)
 *  - fixed rumbling for MS controllers (need 6 bytes output report)
 *  - dropped kernel-2.5 ifdefs, much more readable now
 *  - preparation for major rework under way, stay tuned
 *
 * 2003-03-25 - 0.1.0 : (Franz) Some Debuggin
 *  - Better Handling
 *  - X/Y support, Speed differenting
 *  - Landing Zone, Dead Zone, Offset kompensation, Zero-adjustment, .... aso.
 *  - Removed Wheel handling in Mouse Emulation .. sensless..
 *
 * 2003-01-23 - 0.1.0-pre : added mouse emulation and rumble support
 *  - can provide mouse emulation (compile time switch)
 *    this code has been taken from Oliver Schwartz' xpad-mouse driver
 *  - basic rumble support (compile time switch)        EXPERIMENTAL!  
 *
 * 2002-08-05 - 0.0.6 : added analog button support
 *
 * 2002-07-17 - 0.0.5 : (Vojtech Pavlik) rework
 *  - simplified d-pad handling
 *
 * 2002-07-16 - 0.0.4 : minor changes, merge with Vojtech's v0.0.3
 *  - verified the lack of HID and report descriptors
 *  - verified that ALL buttons WORK
 *  - fixed d-pad to axes mapping
 *
 * 2002-07-14 - 0.0.3 : (Vojtech Pavlik) rework
 *  - indentation fixes
 *  - usb + input init sequence fixes
 *
 * 2002-07-02 - 0.0.2 : basic working version
 *  - all axes and 9 of the 10 buttons work (german InterAct device)
 *  - the black button does not work
 *
 * 2002-06-27 - 0.0.1 : first version, just said "XBOX HID controller"
 */
