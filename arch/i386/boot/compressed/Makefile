#
# linux/arch/i386/boot/compressed/Makefile
#
# create a compressed vmlinux image from the original vmlinux
#

targets		:= vmlinux vmlinux.bin vmlinux.bin.gz head.o misc.o piggy.o

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

LDFLAGS_vmlinux := -Ttext $(IMAGE_OFFSET) -e startup_32

$(obj)/vmlinux: $(obj)/head.o $(obj)/misc.o $(obj)/piggy.o FORCE
	$(call if_changed,ld)
	@:

$(obj)/vmlinux.bin: vmlinux FORCE
	$(call if_changed,objcopy)

$(obj)/vmlinux.bin.gz: $(obj)/vmlinux.bin FORCE
	$(call if_changed,gzip)

LDFLAGS_piggy.o := -r --format binary --oformat elf32-i386 -T

$(obj)/piggy.o: $(obj)/vmlinux.scr $(obj)/vmlinux.bin.gz FORCE
	$(call if_changed,ld)
