# Makefile for simplefs kernel module

# Use the kernel's build system.
# KDIR points to the headers for your currently running kernel.
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# This is the name of our final kernel module object.
obj-m := simplefs.o

# These are the source files that will be compiled and linked into simplefs.o
simplefs-objs := fs.o super.o inode.o dir.o bitmap.o

# The default target, called when you just type 'make'.
all: kernel_module mkfs

# Rule to build the kernel module.
kernel_module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Rule to build the user-space formatting tool.
mkfs: mkfs.simplefs.c
	$(CC) mkfs.simplefs.c -o mkfs.simplefs

# Rule to clean up all compiled files.
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f mkfs.simplefs

.PHONY: all kernel_module mkfs clean