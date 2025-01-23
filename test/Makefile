SRCS=$(shell ls *.c|grep -v t1_)
OBJS=$(SRCS:.c=.o)
TARGETS=$(OBJS:.o=)
CFLAGS := -g -lpthread
CC := gcc

.c.o:
	$(CC) $(CFLAGS) -s -c -o $@ $<

.PHONEY: all

all: $(TARGETS)
	
$(TARGETS): %: %.o
	$(CC) -o $@ $< $(CFLAGS)

clean:
	rm -f $(OBJS) $(TARGETS)
