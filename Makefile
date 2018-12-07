obj-m := bytedriver.o

KDIR := /lib/modules/`uname -r`/build 
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
