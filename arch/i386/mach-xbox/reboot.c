/*
 * arch/i386/mach-xbox/reboot.c 
 * Olivier Fauchon <olivier.fauchon@free.fr>
 * Anders Gustafsson <andersg@0x63.nu>
 *
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

void machine_restart(char * __unused)
{
	printk(KERN_INFO "Sending POWER_RESET to XBOX-PIC.\n");
	xbox_pic_cmd(SMC_SUBCMD_POWER_RESET);  
}

void machine_power_off(void)
{
	printk(KERN_INFO "Sending POWER_OFF to XBOX-PIC.\n");
	xbox_pic_cmd(SMC_SUBCMD_POWER_OFF);  
}

void machine_halt(void)
{
}
