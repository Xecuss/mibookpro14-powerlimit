# SPDX-License-Identifier: GPL-2.0

obj-m := xiaomi-wmi-battery.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
MDIR  := $(PWD)

all:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules_install
	depmod -a

.PHONY: all clean install
