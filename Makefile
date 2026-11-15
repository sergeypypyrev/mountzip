KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD := $(CURDIR)/build
SRCS := Kbuild zipr zipContent $(wildcard *.c *.h)

all:
	mkdir -p $(BUILD)
	cp -R $(addprefix $(CURDIR)/,$(SRCS)) $(BUILD)/
	$(MAKE) -C $(KDIR) M=$(BUILD) modules

clean:
	rm -rf $(BUILD)
