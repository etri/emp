# CURDIR and M used for the first call and nested calls
-include $(CURDIR)/../Makefile.inc
-include $(M)/../Makefile.inc

obj-m := emp.o
emp-y += vm.o mm.o gpa.o hva.o glue.o udma.o
emp-y += alloc.o reclaim.o remote_page.o page_mgmt.o
emp-y += local_page.o donor_mem_rw.o donor_mgmt.o
emp-y += procfs.o
#ifdef CONFIG_EMP_DEBUG
# '#ifdef' adove is not for 'make', but for the code management.
ifeq ($(EMP_DEBUG), yes)
emp-y += debug.o
endif
# '#endif' below is not for 'make', but for the code management.
#endif
ifeq ($(EMP_IO), yes)
 emp-y += iov.o
endif

#ifdef CONFIG_EMP_BLOCKDEV
# '#ifdef' adove is not for 'make', but for the code management.
# for nvme access
ifeq ($(BLOCKDEV_SUPPORT), yes)
emp-y += bdev.o
endif
# '#endif' below is not for 'make', but for the code management.
#endif

#ifdef CONFIG_EMP_RDMA
# '#ifdef' adove is not for 'make', but for the code management.
ifeq ($(RDMA_SUPPORT), yes)
 emp-y += rdma.o
endif
# '#endif' below is not for 'make', but for the code management.
#endif


default: modules
modules:
	make -C $(KDIR) M=$(CURDIR) KBUILD_EXTRA_SYMBOLS="$(KBUILD_EXTRA_SYMBOLS)" modules
distclean: clean cleantags
clean:
	make -C $(KDIR) M=$(CURDIR) clean
	rm -f *.ur-safe
cleantags:
	rm -f cscope.* tags
tags: cleantags
	find $(CURDIR) -name \*.[cshCSH] > cscope.files
	ctags -L cscope.files
	cscope -b -k
install: modules_install

modules_install:
	rm -f $(MODULE_DIR)/emp.ko
	mkdir -p $(MODULE_DIR)
	cp $(CURDIR)/emp.ko $(MODULE_DIR)
	depmod $(shell uname -r)
