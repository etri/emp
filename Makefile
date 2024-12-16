# CURDIR and M used for the first call and nested calls
-include $(CURDIR)/Makefile.inc
-include $(M)/Makefile.inc

ALLDIRS = module_kvm module_gpl
SUBDIRS = module_gpl
ifeq ($(EMP_EXT), yes)
SUBDIRS += module_pro
endif
ifeq ($(RDMA_SUPPORT), yes)
SUBDIRS += vmd
endif
ifeq ($(EMP_VM), yes)
SUBDIRS += module_kvm
endif

.PHONEY: all clean

all: $(SUBDIRS)

module_kvm: FORCE
	$(MAKE) -C $@ $(MAKECMDGOALS)

ifeq ($(EMP_VM), yes)
module_gpl: module_kvm FORCE
	$(MAKE) -C $@ $(MAKECMDGOALS)
else
module_gpl: FORCE
	$(MAKE) -C $@ $(MAKECMDGOALS)
endif

module_pro: module_gpl FORCE
	$(MAKE) -C $@ $(MAKECMDGOALS)

vmd: FORCE
	$(MAKE) -C $@ $(MAKECMDGOALS)

FORCE: ;

clean: $(ALLDIRS)
	rm tags 2>/dev/null || true

install: $(SUBDIRS)

tags: FORCE
	ctags -R
