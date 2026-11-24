# SPDX-License-Identifier: GPL-2.0-or-later
obj-m += tbs6814.o mtv23x.o

tbs6814-y := src/tbsecp3-core.o src/tbsecp3-cards.o src/tbsecp3-i2c.o \
             src/tbsecp3-dma.o src/tbsecp3-dvb.o
mtv23x-y  := src/mtv23x.o

ccflags-y += -I$(src)/src
