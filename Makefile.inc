# Configurations
# set LINUX DISTRO
LINUX_DISTRO := CentOS

# EMP user-level process support
EMP_USER := no
# EMP VM support
EMP_VM := yes
# EMP extension support
EMP_EXT := no
# block and subblock support
EMP_BLOCK := no
# add optimizations
EMP_OPT := no
# io optimization for VM
EMP_IO := yes
# EMP statistics support
EMP_STAT := no
# Include debug codes
EMP_DEBUG := no
# Include instruction emulation
EMP_INSN_EMUL := no
# remote memory using block device support (e.g. NVMe SSD)
BLOCKDEV_SUPPORT := no
# remote memory using memory-like device support (EXPERIMENTAL)
MEMDEV_SUPPORT := no
# remote memory using rdma support
RDMA_SUPPORT := no
#  depend on RDMA_SUPPORT
USE_MLNX_OFED := no
# wss estimator support
WSS_ESTIMATOR := no
#  depend on WSS_ESTIMATOR
WSS_ESTIMATOR_AVL := no
#   depend on WSS_ESTIMATOR and WSS_ESTIMATOR_AVL
WSS_ESTIMATOR_AVL_SHARDS := no
WSS_ESTIMATOR_AVL_PM := no

# Profile memory allocation of KVM (EXPERIMENTAL)
KVM_ALLOC_PROFILE := no
# Prefer DCPMM memory for EMP and/or KVM (VERY EXPERIMENTAL)
# Assume that numa node 0-1 have CPU and DRAM and numa node 2-3 have DCPMM
# EMP_DEBUG should be no
EMP_PREFER_DCPMM := no
KVM_PREFER_DCPMM := no
# for libemp
CUDA_SUPPORT := no

# Internal Variables
CUR_KERN := $(shell uname -r)
CC      := ${CROSS_COMPILE}gcc
AS      := ${CROSS_COMPILE}as
KDIR	:= /lib/modules/$(CUR_KERN)/build
MODULE_DIR := /lib/modules/$(CUR_KERN)/kernel/arch/x86/emp
ifndef M
M := .
endif

# -O0 flags triggers linux kernel bug in using write_unlock
EXTRA_CFLAGS  += -g -Wno-vla
EXTRA_CFLAGS  += -I$(M)/block
EXTRA_CFLAGS  += -I$(M)/../include
# set KVM_DIR
EXTRA_CFLAGS  += -I$(M)/../module_kvm/$(CUR_KERN)
AFLAGS        += -g -D__ASSEMBLY__

# Check requirements of EMP_USER | EMP_VM
ifneq ($(EMP_VM), yes)
	ifneq ($(EMP_USER), yes)
		$(error at least one of EMP_USER and EMP_VM should be enabled)
	endif
endif

# Check requirements of EMP_EXT
ifeq ($(EMP_EXT), yes)
	ifneq ($(EMP_BLOCK), yes)
		$(error EMP_EXT requires EMP_BLOCK)
	endif
	ifneq ($(EMP_OPT), yes)
		$(error EMP_EXT requires EMP_OPT)
	endif
	ifneq ($(EMP_STAT), yes)
		$(error EMP_EXT requires EMP_STAT)
	endif
endif

# Check requirements of device supports
ifneq ($(BLOCKDEV_SUPPORT), yes)
	ifneq ($(MEMDEV_SUPPORT), yes)
		ifneq ($(RDMA_SUPPORT), yes)
			$(error at least one device type should be supported)
		endif
	endif
endif

# Set EXTRA_CFLAGS related to options
ifeq ($(EMP_USER), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_USER
endif
ifeq ($(EMP_VM), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_VM
endif
ifeq ($(EMP_EXT), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_EXT
endif
ifeq ($(EMP_BLOCK), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_BLOCK
endif
ifeq ($(EMP_OPT), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_OPT
endif
ifeq ($(EMP_IO), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_IO
endif
ifeq ($(EMP_STAT), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_STAT
endif
ifeq ($(EMP_DEBUG), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_DEBUG
endif
ifeq ($(EMP_SECURITY), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_SECURITY
endif
ifeq ($(MEMDEV_SUPPORT), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_MEMDEV
endif
ifeq ($(BLOCKDEV_SUPPORT), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_BLOCKDEV
endif
ifeq ($(KVM_ALLOC_PROFILE), yes)
EXTRA_CFLAGS += -DCONFIG_KVM_ALLOC_PROFILE
endif
ifeq ($(EMP_PREFER_DCPMM), yes)
EXTRA_CFLAGS += -DCONFIG_EMP_PREFER_DCPMM
endif

KBUILD_EXTRA_SYMBOLS=
ifeq ($(RDMA_SUPPORT), yes)
 ifeq ($(USE_MLNX_OFED), yes)
  ifneq (${LINUXINCLUDE},)
  LINUXINCLUDE := \
 	-I/usr/src/ofa_kernel/default/include \
 	-include /usr/src/ofa_kernel/default/include/linux/compat-2.6.h \
 	${LINUXINCLUDE}
  endif
  KBUILD_EXTRA_SYMBOLS += /usr/src/ofa_kernel/default/Module.symvers
  EXTRA_CFLAGS += -DMLNX_OFED
 endif
 EXTRA_CFLAGS += -DCONFIG_EMP_RDMA
endif

EXTRA_CFLAGS += -DPRINT_RE_EVENT

ifeq ($(WSS_ESTIMATOR), yes)
EXTRA_CFLAGS += -DSTACK_DIST_PROFILE
KBUILD_EXTRA_SYMBOLS += $(CURDIR)/../wss_estimator/module/Module.symvers
 ifeq ($(WSS_ESTIMATOR_AVL), yes)
 EXTRA_CFLAGS += -DAVL_STACK_DIST -DAVL_STACK_COARSE
  ifeq ($(WSS_ESTIMATOR_AVL_SHARDS), yes)
  EXTRA_CFLAGS += -DAVL_STACK_SHARDS
  endif #WSS_ESTIMATOR_AVL_SHARDS
  ifeq ($(WSS_ESTIMATOR_AVL_PM), yes)
  EXTRA_CFLAGS += -DSLA_PM
  endif #WSS_ESTIMATOR_AVL_PM
 else #DUAL COUNTER STACK
 EXTRA_CFLAGS += -DDUAL_CS
 endif #WSS_ESTIMATOR_AVL
endif #WSS_ESTIMATOR

ifeq ($(REMOTE_INCLUSIVE), yes)
EXTRA_CFLAGS += -DREMOTE_INCLUSIVE
endif
ifeq ($(LINUX_DISTRO), CentOS)
EXTRA_CFLAGS += -DCENTOS
endif

KBUILD_EXTRA_SYMBOLS += $(CURDIR)/../module_gpl/Module.symvers
KBUILD_EXTRA_SYMBOLS += $(CURDIR)/../module_kvm/Module.symvers
