/**
 * Driver that handles the EXTSMI# interrupt on the xbox.
 * Makes it possible to use the eject-button without the xbox rebooting...
 *
 * smbus-command sequence to prevent reboot from cromwell.
 *
 * Changelog:
 *  2003-01-14 Anders Gustafsson <andersg@0x63.nu>
 *             initial version
 *  2003-02-08 Milosch Meriac <xboxlinux@meriac.de>
 *             rewrote debug macros because of compiler errors
 *  2003-08-06 Michael Steil <mist@c64.org>
 *             removed Linux I2C dependency, now compiles
 *             without I2C in the kernel
 *
 * Todo: add errorhandling!
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/io.h>
#include <linux/xbox.h>
#include <linux/interrupt.h>

#define IRQ 12
#define DRIVER_NAME "xboxejectfix"

/* just some crap */
static char dev[]=DRIVER_NAME;

/* External variable from ide-cd.c that specifies whether we simulate drive
   locking in software */
extern volatile int Xbox_simulate_drive_locked;

#define BASE 0x8000

/* Power Management 1 Enable Register */
#define PM02 (BASE+0x02)

/* Power Management 1 Control Register */
#define PM04 (BASE+0x04)

/* ACPI GP Status Register */
#define PM20 (BASE+0x20)

/* ACPI GP Enable Register */
#define PM22 (BASE+0x22)
# define EXTSMI_EN_MASK 0x0002

/* Global SMI Enable Register */
#define PM2A (BASE+0x2A)


static DECLARE_MUTEX(extsmi_sem);
static DECLARE_COMPLETION(extsmi_exited);
static int extsmi_pid=0;

static irqreturn_t extsmi_interupt(int unused, void *dev_id, struct pt_regs *regs) {
	int reason;

	reason=inw(0x8020);
	outw(reason,0x8020); /* ack  IS THIS NEEDED? */
	if(reason&0x2){
		/* wake up thread */
		up(&extsmi_sem);
	}
	return 0;
}

/**
 * Process an event. This is run in process-context.
 */
static void extsmi_process(void){
	int reason;
	reason=Xbox_SMC_read(SMC_CMD_INTERRUPT_REASON);

	if(reason&TRAYBUTTON_MASK){ /* Tray button! Respond to prevent reboot! */
		Xbox_SMC_write(SMC_CMD_INTERRUPT_RESPOND, SMC_SUBCMD_RESPOND_CONTINUE);
		Xbox_SMC_write(0x00, 0x0c);
		/* eject unless lock simulation is being used */
		if (!Xbox_simulate_drive_locked)
			Xbox_tray_eject();
	}
}

static int extsmi_thread(void *data){
	daemonize("extsmi");
	strcpy(current->comm, "xbox_extsmi");

	do {
		extsmi_process();
		down_interruptible(&extsmi_sem);
	} while (!signal_pending(current));
	
         complete_and_exit(&extsmi_exited, 0);
}

static int extsmi_init(void){
	int pid;
	
	if (!machine_is_xbox) {
		printk("This machine is no Xbox.\n");
		return -1;
	}
	printk("Enabling Xbox eject problem workaround.\n");

        pid = kernel_thread(extsmi_thread, NULL,
			    CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	if (pid < 0) {
		return pid;
	}

	extsmi_pid = pid;

	/* this shuts a lot of interrupts off! */
	outw(inw(0x80e2)&0xf8c7,0x80e2);
	outw(0,0x80ac);
	outb(0,0x8025);
	outw(EXTSMI_EN_MASK,PM22); /* enable the EXTSMI# interupt! */
	outw(0,PM02);
	outb(1,PM04); /* enable sci interrupts! */
	Xbox_SMC_write(SMC_CMD_RESET_ON_EJECT, SMC_SUBCMD_RESET_ON_EJECT_DISABLE);

	/* FIXME! retval! */
	request_irq(IRQ,extsmi_interupt,SA_INTERRUPT|SA_SHIRQ,"xboxejectfix",dev);
	return 0;
}

static void extsmi_exit(void){
	int res;
	if (!machine_is_xbox) return; /* can this happen??? */
	free_irq(IRQ,dev);

	/* Kill the thread */
	res = kill_proc(extsmi_pid, SIGTERM, 1);
	wait_for_completion(&extsmi_exited);
	return;
}

module_init(extsmi_init);
module_exit(extsmi_exit);

MODULE_AUTHOR("Anders Gustafsson <andersg@0x63.nu>");
MODULE_LICENSE("GPL");
