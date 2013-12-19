obj-m := fmc_hdd.o

fmc_hdd-objs := hdd_ialloc.o hdd_balloc.o hdd_symlink.o  hdd_super.o  hdd_inode.o  hdd_namei.o  hdd_file.o  hdd_dir.o   hdd_ioctl.o  \
            

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
clean:
	rm -rf *.o *.ko *.mode.c *.markers *.order *.symvers .tmp_versions
