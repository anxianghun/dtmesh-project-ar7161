#
# Copyright (c) 2002-2005 Sam Leffler, Errno Consulting
# Copyright (c) 2002-2005 Atheros Communications, Inc.
# All rights reserved.
#
DEPTH=	../../..

ATHDIR=	${DEPTH}/common/lmac/ath_dev
HALDIR=	${DEPTH}/common/lmac/hal
LINUX_HALDIR=	${DEPTH}/common/lmac/hal/linux
LINUX_INCLUDE=	${DEPTH}/include/linux

ifneq ($(strip $(TOOLPREFIX)),)
CROSS:=$(TOOLPREFIX)
else
CROSS= ${DEPTH}/../build/gcc-3.4.4-2.16.1/build_mips/bin/mips-linux-uclibc-
endif

CC = $(CROSS)gcc

CFLAGS=	-I. -I${LINUX_INCLUDE} -I${HALDIR} -I${LINUX_HALDIR} -I${ATHDIR} -g

ALL=	eeprom rfgain ani txpow reg

all:	${ALL}

eeprom: eeprom.c
	${CC} -o eeprom ${CFLAGS} eeprom.c
rfgain: rfgain.o diag.o
	${CC} -o rfgain ${CFLAGS} rfgain.o diag.o
ani:    ani.o diag.o
	${CC} -o ani ${CFLAGS} ani.o diag.o
txpow:  txpow.c
	${CC} -o txpow ${CFLAGS} txpow.c
reg:	reg.c
	${CC} -o reg ${CFLAGS} reg.c
dumpregs: dumpregs.o dumpregs_5212.o dumpregs_5416.o
	${CC} -o dumpregs ${CFLAGS} dumpregs.o \
		dumpregs_5212.o dumpregs_5416.o

install: ${ALL}
	for i in ${ALL}; do \
		rm -f /usr/local/bin/$$i; \
		cp $$i /usr/local/bin/$$i; \
	done

clean:
	rm -f ${ALL} *.o core
