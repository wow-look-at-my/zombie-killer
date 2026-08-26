# kbuild for the zombie-killer kernel module.
# Build on the target machine:
#   make                 (uses headers for the running kernel)
#   make KDIR=/path/to/kernel/build
obj-m := zombie_killer.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean