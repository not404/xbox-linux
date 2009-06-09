#ifndef __ASM_MACH_PCI_BLACKLIST_H
#define __ASM_MACH_PCI_BLACKLIST_H

static inline int mach_pci_is_blacklisted(int bus, int dev, int fn)
{
	return (bus > 1) || ((bus != 0) && ((dev != 0) || (fn != 0))) ||
		(!bus && !dev && ((fn == 1) || (fn == 2)));
}

#endif
