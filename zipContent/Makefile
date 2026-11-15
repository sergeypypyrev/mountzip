# User space: library and tool         make, make test
# Kernel: demo module zipContentDemo.ko make kmod [KDIR=...]
#
# zipr is expected in ./zipr (a symlink to ../zipr in this tree).
# User-space objects go to build/; kbuild (see Kbuild) builds in place.

CC	?= cc
CFLAGS	?= -O2 -g
CFLAGS	+= -std=c11 -Wall -Wextra -Wshadow
ZIPR	?= zipr
KDIR	?= /lib/modules/$(shell uname -r)/build

B	:= build
SAN	:= -fsanitize=address,undefined -fno-omit-frame-pointer
INC	:= -I. -I$(ZIPR)
LIB_SRC	:= zipContent.c zipContent.h zipContentPort.h
ZIPR_SRC := $(ZIPR)/zipr.c $(ZIPR)/zipr.h $(ZIPR)/ziprPort.h $(ZIPR)/ziprCRC32.h
TOOL	:= tools/zipContentTool.c

all: $(B)/libzipContent.a $(B)/zipContentTool

$(B) $(B)/san:
	mkdir -p $@

$(B)/zipContent.o: $(LIB_SRC) $(ZIPR)/zipr.h | $(B)
	$(CC) $(CFLAGS) $(INC) -c -o $@ zipContent.c

$(B)/zipr.o: $(ZIPR_SRC) | $(B)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $(ZIPR)/zipr.c

$(B)/libzipContent.a: $(B)/zipContent.o $(B)/zipr.o
	$(AR) rcs $@ $^

$(B)/zipContentTool: $(TOOL) zipContent.h $(B)/libzipContent.a
	$(CC) $(CFLAGS) $(INC) -o $@ $(TOOL) $(B)/libzipContent.a

# Test build with sanitizers.
$(B)/san/zipContentTool: $(TOOL) $(LIB_SRC) $(ZIPR_SRC) | $(B)/san
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $(TOOL) zipContent.c $(ZIPR)/zipr.c

test: $(B)/zipContentTool $(B)/san/zipContentTool
	python3 tests/test.py $(B)

kmod:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

kmod-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f $(ZIPR)/zipr.o $(ZIPR)/.zipr.o.cmd

clean:
	rm -rf $(B)

.PHONY: all test kmod kmod-clean clean
