#
# linux/arch/i386/boot/compressed/Makefile
#
# create a compressed vmlinux image from the original vmlinux
#

targets		:= vmlinux vmlinux.bin vmlinux.bin.gz head.o misc.o piggy.o \
			vmlinux.bin.all vmlinux.relocs

#
# There is some strange interaction when paging is off, that makes
# newer v1.1+ Xboxen (manufactured August 2002 or later) crash while
# decrompressing the kernel. Compiling the decrompressor without any
# optimization reliably works around this problem.
#
ifeq ($(CONFIG_X86_XBOX),y)
CFLAGS_misc.o	:= -O0
endif

EXTRA_AFLAGS	:= -traditional

LDFLAGS_vmlinux := -T
hostprogs-y	:= relocs

CFLAGS  := -m32 -D__KERNEL__ $(LINUX_INCLUDE) -O2 \
	   -fno-strict-aliasing -fPIC \
	   $(call cc-option,-ffreestanding) \
	   $(call cc-option,-fno-stack-protector)
LDFLAGS := -m elf_i386

$(obj)/vmlinux: $(src)/vmlinux.lds $(obj)/head.o $(obj)/misc.o $(obj)/piggy.o FORCE
	$(call if_changed,ld)
	@:

$(obj)/vmlinux.bin: vmlinux FORCE
	$(call if_changed,objcopy)

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

$(obj)/piggy.o: $(src)/vmlinux.scr $(obj)/vmlinux.bin.gz FORCE
	$(call if_changed,ld)
