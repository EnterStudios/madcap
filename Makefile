KERNELSRCDIR := /lib/modules/$(shell uname -r)/build
BUILD_DIR := $(shell pwd)
VERBOSE = 0

OVBENCH?=no
flag_ovbench_yes = -DOVBENCH
flag_ovbench_no =

kernel_version=$(shell uname -r | cut -d '-' -f 1)

obj-y := madcap/ raven/ netdevgen/	\
	protocol-drivers-$(kernel_version)/gre/		\
	protocol-drivers-$(kernel_version)/ipip/	\
	protocol-drivers-$(kernel_version)/vxlan/	\
	protocol-drivers-$(kernel_version)/nsh/		\

subdir-ccflags-y := -I$(src)/include $(flag_ovbench_$(OVBENCH))


all:
	make -C $(KERNELSRCDIR) M=$(BUILD_DIR) V=$(VERBOSE) modules

clean:
	make -C $(KERNELSRCDIR) M=$(BUILD_DIR) clean
