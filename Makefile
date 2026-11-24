# Out-of-tree build against the running (or given) kernel.
#   make                 build tbs6814.ko and mtv23x.ko
#   make KVER=x.y        build against another installed kernel
#   make clean
#   make dkms-install    install git HEAD into /usr/src and build/install via DKMS (sudo)
#   make dkms-uninstall  remove this version from DKMS (sudo)
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

DKMS_NAME    := $(shell sed -n 's/^PACKAGE_NAME="\(.*\)"/\1/p' dkms.conf)
DKMS_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DKMS_SRC     := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

dkms-check:
	@test -n "$(DKMS_NAME)" && test -n "$(DKMS_VERSION)" || \
		{ echo "error: could not parse name/version from dkms.conf"; exit 1; }

# Installs the committed tree (git HEAD), never uncommitted changes.
dkms-install: dkms-check
	@test -z "$$(git status --porcelain)" || \
		{ echo "error: working tree is dirty, commit first"; exit 1; }
	@# unregister every installed version: they all own the same module
	@# files. Old /usr/src/$(DKMS_NAME)-<version> trees are left in place.
	@for v in $$(dkms status $(DKMS_NAME) | sed -n 's|^$(DKMS_NAME)/\([^,]*\),.*|\1|p' | sort -u); do \
		echo "dkms remove $(DKMS_NAME)/$$v"; \
		sudo dkms remove "$(DKMS_NAME)/$$v" --all; \
	done
	sudo rm -rf --one-file-system "$(DKMS_SRC)"
	sudo mkdir -p "$(DKMS_SRC)"
	git archive HEAD | sudo tar -x -C "$(DKMS_SRC)"
	sudo dkms install $(DKMS_NAME)/$(DKMS_VERSION)

dkms-uninstall: dkms-check
	-sudo dkms remove $(DKMS_NAME)/$(DKMS_VERSION) --all
	sudo rm -rf --one-file-system "$(DKMS_SRC)"

.PHONY: all clean dkms-check dkms-install dkms-uninstall
