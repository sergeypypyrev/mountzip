# User space: library and tool         make, make test, make test-big
# Kernel: demo module ziprDemo.ko     make kmod [KDIR=...]
#
# User-space objects go to build/; kbuild (see Kbuild) builds in place.

CC	?= cc
CFLAGS	?= -O2 -g
CFLAGS	+= -std=c11 -Wall -Wextra -Wshadow
KDIR	?= /lib/modules/$(shell uname -r)/build

B	:= build
SAN	:= -fsanitize=address,undefined -fno-omit-frame-pointer
LIB_SRC	:= zipr.c zipr.h ziprPort.h ziprCRC32.h
TOOL	:= tools/ziprTool.c

all: $(B)/libzipr.a $(B)/ziprTool

$(B) $(B)/san $(B)/zlib:
	mkdir -p $@

$(B)/zipr.o: $(LIB_SRC) | $(B)
	$(CC) $(CFLAGS) -c -o $@ zipr.c

$(B)/libzipr.a: $(B)/zipr.o
	$(AR) rcs $@ $^

$(B)/ziprTool: $(TOOL) zipr.h $(B)/libzipr.a
	$(CC) $(CFLAGS) -o $@ $(TOOL) $(B)/libzipr.a

# Test builds with sanitizers. The zlib one enables deflate through the
# system zlib to exercise the inflate code that the kernel build uses.
$(B)/san/ziprTool: $(TOOL) $(LIB_SRC) | $(B)/san
	$(CC) $(CFLAGS) $(SAN) -o $@ $(TOOL) zipr.c

$(B)/zlib/ziprTool: $(TOOL) $(LIB_SRC) | $(B)/zlib
	$(CC) $(CFLAGS) $(SAN) -DZIPR_TEST_ZLIB -o $@ $(TOOL) zipr.c -lz

test: $(B)/ziprTool $(B)/san/ziprTool $(B)/zlib/ziprTool
	tests/run.sh $(B)

# 12 GiB sparse member and 10 million entries; needs about 1 GB of disk.
test-big: $(B)/ziprTool
	python3 tests/big.py $(B)

kmod:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

kmod-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

clean:
	rm -rf $(B)

.PHONY: all test test-big kmod kmod-clean clean
