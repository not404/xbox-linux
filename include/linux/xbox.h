#ifndef _XBOX_H_
#define _XBOX_H_

#define XBOX_SMB_IO_BASE 0xC000
#define XBOX_SMB_HOST_ADDRESS       (0x4 + XBOX_SMB_IO_BASE)
#define XBOX_SMB_HOST_COMMAND       (0x8 + XBOX_SMB_IO_BASE)
#define XBOX_SMB_HOST_DATA          (0x6 + XBOX_SMB_IO_BASE)
#define XBOX_SMB_GLOBAL_ENABLE       (0x2 + XBOX_SMB_IO_BASE)
#define XBOX_GE_CYC_TYPE_MASK (7)
#define XBOX_BYTE_DATA    0x02

#define XBOX_SMC_ADDRESS 0x10
#define XBOX_TV_ADDRESS 0x45

#define SMC_CMD_POWER 0x02
#define SMC_CMD_TRAY_STATE 0x03
#define SMC_CMD_AV_PACK 0x04
#define SMC_CMD_LED_MODE 0x07
#define SMC_CMD_LED_REGISTER 0x08
#define SMC_CMD_EJECT 0x0C
#define SMC_CMD_INTERRUPT_RESPOND 0x0D
#define SMC_CMD_INTERRUPT_REASON 0x11
#define SMC_CMD_RESET_ON_EJECT 0x19
#define SMC_CMD_SCRATCH_REGISTER 0x1B
// I think commands 20 and 21 are used for bootup authentication, but
// I don't know those commands.  The CROM people know.
//
#define SMC_SUBCMD_POWER_RESET 0x01
#define SMC_SUBCMD_POWER_CYCLE 0x40
#define SMC_SUBCMD_POWER_OFF 0x80
//
#define SMC_SUBCMD_RESPOND_CONTINUE 0x04
//
// These are from recent posts to this list (except MISSING)
#define SMC_VALUE_AV_SCART 0x00
#define SMC_VALUE_AV_HDTV 0x01
#define SMC_VALUE_AV_VGA 0x02
#define SMC_VALUE_AV_RFU 0x03
#define SMC_VALUE_AV_SVIDEO 0x04
#define SMC_VALUE_AV_STANDARD 0x06
#define SMC_VALUE_AV_UNDEFINED 0x05
#define SMC_VALUE_AV_MISSING 0x07
//
#define SMC_SUBCMD_LED_MODE_DEFAULT 0x00
#define SMC_SUBCMD_LED_MODE_CUSTOM 0x01

#define SMC_SUBCMD_EJECT_EJECT 0x00
#define SMC_SUBCMD_EJECT_LOAD 0x01

// Bits 01...40 all have meaning but I don't know them all.
#define SMC_VALUE_INTERRUPT_POWER_BUTTON 0x01
#define SMC_VALUE_INTERRUPT_AV_REMOVED 0x10
#define SMC_VALUE_INTERRUPT_EJECT_BUTTON 0x20

#define SMC_SUBCMD_RESET_ON_EJECT_ENABLE 0x00
#define SMC_SUBCMD_RESET_ON_EJECT_DISABLE 0x01

// These are defined by the *kernel*, not the SMC.
#define SMC_SCRATCH_EJECT_AFTER_BOOT 0x01
#define SMC_SCRATCH_DISPLAY_ERROR 0x02
#define SMC_SCRATCH_NO_ANIMATION 0x04
#define SMC_SCRATCH_RUN_DASHBOARD 0x08

/* interrupt causes */
#define POWERDOWN_MASK (1<<0)
#define TRAYCLOSED_MASK (1<<1)
#define TRAYOPENING_MASK (1<<2)
#define AVPLUGGED_MASK (1<<3)
#define AVUNPLUGGED_MASK (1<<4)
#define TRAYBUTTON_MASK (1<<5)
#define TRAYCLOSING_MASK (1<<6)
#define UNKNOWN_MASK (1<<7)

extern int machine_is_xbox;

#define XBOX_I2C_IO_BASE 0xc000
static inline void Xbox_SMC_write(u8 d1,u8 d2) {
	int c=4;
	u8 b=0;
	u32 dwSpinsToLive = 0x8000000;

	/*
	while(inw(XBOX_I2C_IO_BASE+0)&0x0800);
	*/
	while(c--) {
		outb(XBOX_SMC_ADDRESS<<1, XBOX_SMB_HOST_ADDRESS);
		outb((u8)d1, XBOX_SMB_HOST_COMMAND);
		outb((u8)d2, XBOX_SMB_HOST_DATA);
		outw(0xffff, XBOX_I2C_IO_BASE+0);
		outb(0x0a, XBOX_SMB_GLOBAL_ENABLE);
		{
			while((b !=0x10) && ((b&0x26)==0) && (dwSpinsToLive--)) {
				b=inb(XBOX_I2C_IO_BASE);
			}
			if(b&0x2) continue;
			if(b&0x24) continue;
			if(!(b&0x10)) continue;
			break;
		}
	}
}

static inline int Xbox_SMC_read(u8 d) {
        int c=4;
	u8 b=0;
	u32 dwSpinsToLive = 0x8000000;
	
	/*
	while(inw(XBOX_I2C_IO_BASE+0)&0x0800);
	*/
        while(c--) {
		outb((XBOX_SMC_ADDRESS<<1)|1, XBOX_SMB_HOST_ADDRESS);
                outb(d, XBOX_SMB_HOST_COMMAND);
                outw(0xffff, XBOX_I2C_IO_BASE+0);
                outb(0x0a, XBOX_SMB_GLOBAL_ENABLE);
		{
			while((b !=0x10) && ((b&0x26)==0) && (dwSpinsToLive--)) {
				b=inb(XBOX_I2C_IO_BASE);
			}
			if(b&0x2) continue;
			if(b&0x24) continue;
			if(!(b&0x10)) continue;
			break;	
		}
	}
	return (int)inb(XBOX_SMB_HOST_DATA);
}

#define Xbox_tray_load() Xbox_SMC_write(SMC_CMD_EJECT, SMC_SUBCMD_EJECT_LOAD);
#define Xbox_tray_eject() Xbox_SMC_write(SMC_CMD_EJECT, SMC_SUBCMD_EJECT_EJECT);

#define Xbox_power_off() Xbox_SMC_write(SMC_CMD_POWER, SMC_SUBCMD_POWER_OFF);
#define Xbox_reset() Xbox_SMC_write(SMC_CMD_POWER, SMC_SUBCMD_POWER_RESET);

#endif /* _XBOX_H_ */
