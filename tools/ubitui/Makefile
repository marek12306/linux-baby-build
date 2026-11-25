# ubitui: cfdisk-like terminal UI for UBI volumes.
#
# Links libubi, libmtd, libubigen and the CRC32 code from the mtd-utils
# sources in MTD_SRC (build.sh fetches mtd-utils 2.2.1 and cross-compiles a
# static binary for the radio). A native build only checks compilation:
#   make MTD_SRC=/path/to/mtd-utils-2.2.1 O=/tmp/ubitui-host
CC ?= cc
MTD_SRC ?= mtd-utils
O ?= .
CFLAGS ?= -Os
CFLAGS += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-sign-compare \
	-I$(MTD_SRC)/include -DWITHOUT_XATTR
LDFLAGS ?=

OBJS := main.o term.o ui.o ubi.o flash.o md5.o
LIBOBJS := libubi.o libmtd.o libmtd_legacy.o libubigen.o libcrc32.o common.o

$(O)/ubitui: $(addprefix $(O)/,$(OBJS) $(LIBOBJS))
	$(CC) $(LDFLAGS) -o $@ $^

$(O)/%.o: %.c term.h ui.h ubi.h flash.h md5.h
	$(CC) $(CFLAGS) -c -o $@ $<

# mtd-utils code: its own warnings are not ours to fix
$(O)/%.o: $(MTD_SRC)/lib/%.c
	$(CC) $(CFLAGS) -w -DVERSION='"2.2.1"' -I$(MTD_SRC)/lib -c -o $@ $<

clean:
	rm -f $(O)/ubitui $(addprefix $(O)/,$(OBJS) $(LIBOBJS))

.PHONY: clean
