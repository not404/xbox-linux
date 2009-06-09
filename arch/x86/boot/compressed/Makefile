#
# linux/arch/x86/boot/compressed/Makefile
#
# create a compressed vmlinux image from the original vmlinux
#

targets := vmlinux vmlinux.bin vmlinux.bin.gz head_$(BITS).o misc.o piggy.o

#
# There is some strange interaction when paging is off, that makes
# newer v1.1+ Xboxen (manufactured August 2002 or later) crash while
# decrompressing the kernel. Compiling the decrompressor without any
# optimization reliably works around this problem.
#
ifeq ($(CONFIG_X86_XBOX),y)
CFLAGS_misc.o   := -O0
endif

KBUILD_CFLAGS := -m$(BITS) -D__KERNEL__ $(LINUX_INCLUDE) -O2
KBUILD_CFLAGS += -fno-strict-aliasing -fPIC
cflags-$(CONFIG_X86_64) := -mcmodel=small
KBUILD_CFLAGS += $(cflags-y)
KBUILD_CFLAGS += $(call cc-option,-ffreestanding)
KBUILD_CFLAGS += $(call cc-option,-fno-stack-protector)

KBUILD_AFLAGS  := $(KBUILD_CFLAGS) -D__ASSEMBLY__

LDFLAGS := -m elf_$(UTS_MACHINE)
LDFLAGS_vmlinux := -T

$(obj)/vmlinux: $(src)/vmlinux_$(BITS).lds $(obj)/head_$(BITS).o $(obj)/misc.o $(obj)/piggy.o FORCE
	$(call if_changed,ld)
	@:

OBJCOPYFLAGS_vmlinux.bin := -O binary -R .note -R .comment -S
$(obj)/vmlinux.bin: vmlinux FORCE
	$(call if_changed,objcopy)


ifeq ($(CONFIG_X86_32),y)
targets += vmlinux.bin.all vmlinux.relocs
hostprogs-y := relocs

quiet_cmd_relocs = RELOCS  $@
      cmd_relocs = $(obj)/relocs $< > $@;$(obj)/relocs --abs-relocs $<
$(obj)/vmlinux.relocs: vmlinux $(obj)/relocs FORCE
	$(call if_changed,relocs)

vmlinux.bin.all-y := $(obj)/vmlinux.bin
vmlinux.bin.all-$(CONFIG_RELOCATABLE) += $(obj)/vmlinux.relocs
quiet_cmd_relocbin = BUILD   $@
      cmd_relocbin = cat $(filter-out FORCE,$^) > $@
$(obj)/vmlinux.bin.all: $(vmlinux.bin.all-y) FORCE
	$(call if_changed,relocbin)

ifdef CONFIG_RELOCATABLE
$(obj)/vmlinux.bin.gz: $(obj)/vmlinux.bin.all FORCE
	$(call if_changed,gzip)
else
$(obj)/vmlinux.bin.gz: $(obj)/vmlinux.bin FORCE
	$(call if_changed,gzip)
endif
LDFLAGS_piggy.o := -r --format binary --oformat elf32-i386 -T

else
$(obj)/vmlinux.bin.gz: $(obj)/vmlinux.bin FORCE
	$(call if_changed,gzip)

LDFLAGS_piggy.o := -r --format binary --oformat elf64-x86-64 -T
endif


$(obj)/piggy.o: $(obj)/vmlinux.scr $(obj)/vmlinux.bin.gz FORCE
	$(call if_changed,ld)
