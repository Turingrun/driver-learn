obj-m += mychar/
obj-m += globalmem/
obj-m += globalfifo/


KDIR := ~/kernel/Linux5.10
ARCH := arm64
CROSS_COMPILE := aarch64-none-linux-gnu-
all:
	$(MAKE) -C $(KDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) \
		modules

clean:
	$(MAKE) -C $(KDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) \
		clean