/*
 * arch/i386/mach-xbox/reboot.c 
 *
 * Power-off/reboot mach hooks.
 * 
 * Copyright (C) 2003  Olivier Fauchon <olivier.fauchon@free.fr>
 * Copyright (C) 2003  Anders Gustafsson <andersg@0x63.nu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/io.h>

/* we don't use any of those, but dmi_scan.c needs 'em */
void (*pm_power_off)(void);
int reboot_thru_bios;

#define XBOX_SMB_IO_BASE		0xC000
#define XBOX_SMB_HOST_ADDRESS		(0x4 + XBOX_SMB_IO_BASE)
#define XBOX_SMB_HOST_COMMAND		(0x8 + XBOX_SMB_IO_BASE)
#define XBOX_SMB_HOST_DATA		(0x6 + XBOX_SMB_IO_BASE)
#define XBOX_SMB_GLOBAL_ENABLE		(0x2 + XBOX_SMB_IO_BASE)

#define XBOX_PIC_ADDRESS		0x10

#define SMC_CMD_POWER			0x02
#define SMC_SUBCMD_POWER_RESET		0x01
#define SMC_SUBCMD_POWER_CYCLE		0x40
#define SMC_SUBCMD_POWER_OFF		0x80


static void xbox_pic_cmd(u8 command)
{
	outw_p(((XBOX_PIC_ADDRESS) << 1), XBOX_SMB_HOST_ADDRESS);
	outb_p(SMC_CMD_POWER, XBOX_SMB_HOST_COMMAND);
	outw_p(command, XBOX_SMB_HOST_DATA);
	outw_p(inw(XBOX_SMB_IO_BASE), XBOX_SMB_IO_BASE);
	outb_p(0x0a, XBOX_SMB_GLOBAL_ENABLE);
}

void machine_emergency_restart(void)
{
	xbox_pic_cmd(SMC_SUBCMD_POWER_CYCLE);  
}

void machine_restart(char *cmd)
{
	machine_emergency_restart();
}

void machine_shutdown(void)
{
	/* Subarch-specific shutdown prior to a kexec */
}

void machine_power_off(void)
{
	xbox_pic_cmd(SMC_SUBCMD_POWER_OFF);  
}

void machine_halt(void)
{
}
