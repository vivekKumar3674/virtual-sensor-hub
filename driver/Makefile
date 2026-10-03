# Out-of-tree kernel module build.
#   make            build vsensor.ko against the running kernel
#   make clean
# Needs kernel headers: sudo apt install build-essential linux-headers-$(uname -r)
obj-m += vsensor.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
