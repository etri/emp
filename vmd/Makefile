MLNX_OFED=no

SRCS=vmd.c
OBJS=$(SRCS:.c=.o)
TARGETS=$(OBJS:.o=)
CFLAGS := -g -libverbs -lrdmacm -lpthread -Wall
ifeq ($(MLNX_OFED), yes)
 CFLAGS += -DMLNX_OFED
endif

.c.o:
	gcc $(CFLAGS) -c -o $@ $<

.PHONEY: all

all: $(TARGETS)
	
$(TARGETS): %: %.o
	gcc -o $@ $< $(CFLAGS)

clean:
	rm -f $(OBJS) $(TARGETS)

install:
	mkdir -p /usr/local/bin/emp
	cp $(TARGETS) /usr/local/bin/emp/
