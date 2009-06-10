#
# Makefile for the Linux fat filesystem support.
#

obj-$(CONFIG_FAT_FS) += fat.o
obj-$(CONFIG_VFAT_FS) += vfat.o
obj-$(CONFIG_MSDOS_FS) += msdos.o
obj-$(CONFIG_FATX_FS) += fatx.o


fat-y := cache.o dir.o fatent.o file.o inode.o misc.o
vfat-y := namei_vfat.o
msdos-y := namei_msdos.o
fatx-y := namei_fatx.o
