# Out-of-tree build against the running (or given) kernel.
#   make            build tbs6814.ko and mtv23x.ko
#   make KVER=x.y   build against another installed kernel
#   make clean
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
