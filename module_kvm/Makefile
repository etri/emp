# place kvm-related directories to $(KERN_VERSION) and its subdir common
# $(KDIR)/arch/x86/kvm => $(KERN_VERSION)
# $(KDIR)/virt/kvm => $(KERN_VERSION)/common/

# CURDIR and M used for the first call and nested calls
-include $(CURDIR)/../Makefile.inc
-include $(M)/../Makefile.inc

KBUILD_EXTRA_SYMBOLS=

ifndef KVM_VERSION
CUR_KERN = $(shell uname -r)
else
CUR_KERN = $(KVM_VERSION)
endif
obj-y   += $(CUR_KERN)/

# following KVM_DIR and KVM_EXTRA_CFLAGS required for compiling KVM
KVM_DIR=$(shell realpath $(CURDIR)/$(CUR_KERN))
ifeq ($(KVM_PREFER_DCPMM), yes)
KVM_EXTRA_CFLAGS += -DCONFIG_KVM_PREFER_DCPMM
endif
ifeq ($(KVM_ALLOC_PROFILE), yes)
KVM_EXTRA_CFLAGS += -DCONFIG_KVM_ALLOC_PROFILE
endif
KVM_EXTRA_CFLAGS += -DCONFIG_KVM_EMP -I$(KVM_DIR) -I$(CURDIR)/../include
# end of addition

CONFIG_KVM = m
CONFIG_KVM_INTEL = m
CONFIG_KVM_AMD = m

default: modules
modules: prepare
	make -C $(KDIR) M=$(CURDIR) KVM_DIR=$(KVM_DIR) EXTRA_CFLAGS="$(KVM_EXTRA_CFLAGS)" modules

install:
	mkdir -p /lib/modules/$(CUR_KERN)/kernel/arch/x86/emp 2> /dev/null
	find $(CURDIR) -name \*.ko \
		-exec cp {} /lib/modules/$(CUR_KERN)/kernel/arch/x86/emp/ \;
	depmod $(CUR_KERN)

uninstall:
	rm -rf /lib/modules/$(CUR_KERN)/kernel/arch/x86/emp

distclean: clean cleantags

clean:
	test ! -d $(KVM_DIR) || make -C $(KDIR) M=$(CURDIR) KVM_DIR=$(KVM_DIR) clean
	rm -f *.ur-safe

cleantags:
	rm -f cscope.* tags

tags: cleantags
	find $(CURDIR) -name \*.[cshCSH] > cscope.files
	ctags -L cscope.files
	cscope -b -k

prepare:
	./make_link.sh
