/*
 * Xbox input device driver for Linux - v0.1.4
 *
 *	mouse emulation stuff, merged from Olivers xpad-mouse
 *
 * Copyright (c)  2003, 2004  Marko Friedemann <mfr@bmx-chemnitz.de>
 *	portions Copyright (c)	2002  Oliver Schwartz <Oliver.Schwartz@gmx.de>,
 *				2003  Franz Lehner <franz@chaos.at>
 *
 * Released under GPL. See xpad-core.c for details
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/proc_fs.h>
#include <linux/usb.h>
#include <linux/version.h>
#include <linux/timer.h>
#include <asm/uaccess.h>

#define __USB_XPAD_MOUSE
#include "xpad.h"
#undef __USB_XPAD_MOUSE

#define XPAD_WHEELBRAKE		20
#define JOY_DeadZone_fast	6000
#define JOY_DeadZone_slow	200
#define XPAD_OFFSET_COUNTER	5


static int mouse_on_load = 1;
MODULE_PARM( mouse_on_load, "i" );
MODULE_PARM_DESC( mouse_on_load, "set to 0 to deactivate mouse support on insmod (default 1)" );


/**
 *	xpad_removedeadzone
 *
 *	Franz? Please clarify and correct.
 *
 *	Removes the deadzone for mouse operation.
 *	Allows for better handling near the stick's center.
 */   
static int xpad_mouse_removedeadzone(signed int position, int speed, int deadzone)
{
	if (position>31000) position=31000;
	if (position<-31000) position=-31000; 
	
	if ((position>0)&(position<deadzone)) return 0;
	if ((position<0)&(position>(-deadzone))) return 0;

	if (position>deadzone) position -= deadzone;	
	if (position<(-(deadzone))) position+= deadzone;	
	position = (int)(position / speed);

	return position;
}

void xpad_mouse_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
        struct input_dev *dev_mouse = &xpad->dev_mouse;
	
        int signledirection, xyspeed;
        //int joy_y2;
	unsigned char MouseLeft,MouseRight, MouseMiddle;
        signed int left_joy_x, left_joy_y, right_joy_x, right_joy_y;
	
	if (!xpad->mouse_open_count || !xpad->mouse_enabled)
		return;
	
	left_joy_x = ((__s16) (((__s16)data[13] << 8) | data[12]));
	left_joy_y = ((__s16) (((__s16)data[15] << 8) | data[14]));
	
	right_joy_x = ((__s16) (((__s16)data[17] << 8) | data[16]));
	right_joy_y = ((__s16) (((__s16)data[19] << 8) | data[18]));
	
	// Creates Offset when first starting
	/* CHECKME: who coded this? Franz? Please clarify:
		1) is this necessary for joystick operation?
		2) offset_counter was only defined when MOUSE
		   support was configured (has been FIXED, see above) */
	if (xpad->offsetset_compensation>0) {
		
		if (xpad->offsetset_compensation == XPAD_OFFSET_COUNTER) {
			xpad->left_offset_x  = left_joy_x;
			xpad->left_offset_y  = left_joy_y;
			xpad->right_offset_x = right_joy_x;
			xpad->right_offset_y = right_joy_y;  
		} else {
			xpad->left_offset_x  += left_joy_x;
			xpad->left_offset_y  += left_joy_y;
			xpad->right_offset_x += right_joy_x;
			xpad->right_offset_y += right_joy_y;  
		}
		
		if (xpad->offsetset_compensation == 1) {
			xpad->left_offset_x  = xpad->left_offset_x  / XPAD_OFFSET_COUNTER;
			xpad->left_offset_y  = xpad->left_offset_y  / XPAD_OFFSET_COUNTER;
			xpad->right_offset_x = xpad->right_offset_x / XPAD_OFFSET_COUNTER;
			xpad->right_offset_y = xpad->right_offset_y / XPAD_OFFSET_COUNTER;  
		}
		
		xpad->offsetset_compensation--;
	}
	
	left_joy_x -= xpad->left_offset_x;
	left_joy_y -= xpad->left_offset_y;
	
	right_joy_x -= xpad->right_offset_x;
	right_joy_y -= xpad->right_offset_y;
	
	if (data[11]<0x10) {
		// Normal Speed Mode
		xpad->rel_x =  (xpad_mouse_removedeadzone(left_joy_x,0x1500,JOY_DeadZone_fast));
		xpad->rel_y = -(xpad_mouse_removedeadzone(left_joy_y,0x1500,JOY_DeadZone_fast));
		xyspeed = 2;
		//printk("%d:",xpad->rel_y);
	} else {
		// Ultra Slow Mode                                                 
		xpad->rel_x =  (xpad_mouse_removedeadzone(left_joy_x,0x3500,JOY_DeadZone_slow));
		xpad->rel_y = -(xpad_mouse_removedeadzone(left_joy_y,0x3500,JOY_DeadZone_slow));    
		xyspeed = 1;
	}
	
	// X-Y Steering
	signledirection=1;
	if (signledirection&((data[2] & 0x04)!=0)) { signledirection=0; xpad->rel_x -=xyspeed; }
	if (signledirection&((data[2] & 0x08)!=0)) { signledirection=0; xpad->rel_x +=xyspeed; }
	if (signledirection&((data[2] & 0x02)!=0)) { signledirection=0; xpad->rel_y +=xyspeed; }
	if (signledirection&((data[2] & 0x01)!=0)) { signledirection=0; xpad->rel_y -=xyspeed; }
  	
	/* wheel handling */
	//joy_y2 = xpad_mouse_removedeadzone(joy_y2);
	//xpad->rel_wheel = (joy_y2>0)?1:(joy_y2<0)?-1:0;
	xpad->rel_wheel=0;
	
	if (data[10]==0xFF) MouseLeft=1; else MouseLeft =0;
	if ((MouseLeft==0)&(data[7]!=0)) MouseLeft =1;
	if ((MouseLeft==0)&(data[4]!=0)) MouseLeft = 1;
	if ((MouseLeft==0)&((data[2] >> 7)!=0)) MouseLeft = 1;
	if ((MouseLeft==0)&(((data[2] & 0x40) >> 6)!=0)) MouseLeft = 1;
	
	if (data[5]!=0) MouseRight=1; else MouseRight=0;
	if (data[6]!=0) MouseMiddle =1; else MouseMiddle=0;
	
	// Generating Mouse Emulation Events  (Button Events)
	input_report_key(dev_mouse, BTN_LEFT, MouseLeft);
	input_report_key(dev_mouse, BTN_RIGHT, MouseRight);
	input_report_key(dev_mouse, BTN_MIDDLE, MouseMiddle);
	
	input_sync(dev_mouse);
}

/**
 *	xpad_timer
 *
 *	Reports the mouse events in the interval given in xpad_open.
 *
 *	Taken from Oliver Schwartz' xpad-mouse driver to avoid strange mouse
 *	 behaviour encountered when the input events where send directly
 *	 in xpad_process_packet.
 */
static void xpad_timer(unsigned long data)
{
	struct usb_xpad * xpad = (struct usb_xpad *)data;

	if (xpad->mouse_enabled) {
		input_report_rel(&xpad->dev_mouse, REL_X, xpad->rel_x);
		input_report_rel(&xpad->dev_mouse, REL_Y, xpad->rel_y); 
		
		input_sync(&xpad->dev_mouse);
	
		/*if (xpad->rel_wheeltimer == 0) {
			input_report_rel(&xpad->dev_mouse, REL_WHEEL, xpad->rel_wheel);
			xpad->rel_wheeltimer = XPAD_WHEELBRAKE;
		} else
			xpad->rel_wheeltimer--;*/
	}
	
	// reschedule the timer so that it fires continually
	add_timer(&xpad->timer);
}

static int xpad_mouse_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = dev->private;
	int status;
	
	if (xpad->mouse_open_count)
		return 0;
	
	if ((status = xpad_start_urb(xpad)))
		return status;
		
	++xpad->mouse_open_count;
	
	info("opening mouse device");

	// set up timer for mouse event generation
	init_timer(&xpad->timer);
	xpad->timer.expires = 1*HZ/5; /* every 200 ms */
	xpad->timer.data = (unsigned long)xpad;
	xpad->timer.function = xpad_timer;
	// now start the timer
	add_timer(&xpad->timer);
	
	return 0;
}

static void xpad_mouse_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = dev->private;
	
	if (--xpad->mouse_open_count)
		return;
		
	xpad_stop_urb(xpad);
	
	info("closing mouse device"); 
	del_timer(&xpad->timer);
}

int xpad_mouse_init_input_device(struct usb_interface *intf, struct xpad_device device)
{
	struct usb_xpad *xpad = usb_get_intfdata(intf);
	struct usb_device *udev = interface_to_usbdev(intf);

	/* the mouse device struct for the kernel (mouse emulation) */
	xpad->dev_mouse.id.bustype = BUS_USB;
	xpad->dev_mouse.id.vendor = udev->descriptor.idVendor;
	xpad->dev_mouse.id.product = udev->descriptor.idProduct;
	xpad->dev_mouse.id.version = udev->descriptor.bcdDevice;
	xpad->dev_mouse.dev = &intf->dev;
	xpad->dev_mouse.private = xpad;
	xpad->dev_mouse.name = device.name;
	xpad->dev_mouse.phys = xpad->phys;
	xpad->dev_mouse.open = xpad_mouse_open;
	xpad->dev_mouse.close = xpad_mouse_close;
	xpad->offsetset_compensation = XPAD_OFFSET_COUNTER; // Find new offset point
	xpad->mouse_enabled = mouse_on_load;
	
	/* mouse setup */
	xpad->dev_mouse.evbit[0] = BIT(EV_KEY) | BIT(EV_REL);

	set_bit(REL_X,     xpad->dev_mouse.relbit);
	set_bit(REL_Y,     xpad->dev_mouse.relbit);
	set_bit(REL_WHEEL, xpad->dev_mouse.relbit);

	set_bit(BTN_LEFT,   xpad->dev_mouse.keybit);
	set_bit(BTN_RIGHT,  xpad->dev_mouse.keybit);
	set_bit(BTN_MIDDLE, xpad->dev_mouse.keybit);
	set_bit(BTN_SIDE,   xpad->dev_mouse.keybit);
	set_bit(BTN_EXTRA,  xpad->dev_mouse.keybit);
	
	input_register_device(&xpad->dev_mouse);
	printk(KERN_INFO "input: mouse emulation %s@ %s\n",
	       (mouse_on_load ? "" : "(disabled) "), xpad->dev_mouse.name);
}

void xpad_mouse_cleanup(struct usb_xpad *xpad)
{
	del_timer(&xpad->timer);
	input_unregister_device(&xpad->dev_mouse);
}
