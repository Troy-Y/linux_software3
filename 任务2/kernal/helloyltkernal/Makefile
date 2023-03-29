KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
obj-m := helloyltkernal.o	
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.mod *.order *.symvers
