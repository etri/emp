-include $(CURDIR)/../Makefile.inc

TARGET=libemp.so

CFLAGS := -I../include/ -I./ -g -Wl,--no-as-needed -ldl -fPIC -lnuma
LDFLAGS := -shared

SRCS=$(shell ls *.c|grep -v cuda)
HDRS=$(shell ls *.h)
OBJS=$(SRCS:.c=.o)
	
ifeq ($(CUDA_SUPPORT), yes)
CFLAGS += -I/usr/local/cuda/include -DCONFIG_CUDA_SUPPORT
LDFLAGS += -L/usr/local/cuda/lib64 -lcudart

SRCS+=$(shell ls *cuda*.c)
endif

.PHONEY: all

all: $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	
$(TARGET) : $(OBJS)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
