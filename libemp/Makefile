-include $(CURDIR)/../Makefile.inc

TARGET=libemp.so

CFLAGS := -I../include/ -I./ -g -Wl,--no-as-needed -ldl -fPIC -lnuma
LDFLAGS := -shared

SRCS=$(shell ls *.c)
HDRS=$(shell ls *.h)
OBJS=$(SRCS:.c=.o)
	
.PHONEY: all

all: $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	
$(TARGET) : $(OBJS)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
