#
# Makefile for the Linux fatx filesystem routines.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile.

#
# Makefile for the Linux msdos filesystem routines.
#

obj-$(CONFIG_FATX_FS) += fatx.o

fatx-objs := namei.o cache.o dir.o file.o inode.o misc.o fatxfs_syms.o
