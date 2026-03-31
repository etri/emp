MPICC   = /usr/lib64/openmpi/bin/mpicc
MPIRUN  = /usr/lib64/openmpi/bin/mpirun

EMP     = EMP_MEM_PATH="dram:$$((16<<10))|10.10.0.113:32768:20675|10.10.0.113:32768:20676|10.10.0.113:32768:20677|10.10.0.113:32768:20678|10.10.0.113:32768:20679|10.10.0.113:32768:20680|10.10.0.113:32768:20681|10.10.0.113:32768:20682" \
          LD_PRELOAD=/usr/local/lib64/libmimalloc.so
MPIOPT  = --mca btl self,vader --mca mtl ^ofi \
          -x EMP_MEM_PATH -x LD_PRELOAD

TARGETS = t1_hello t2_malloc t3_fork t4_collective t5_manyranks t6_thread

all: $(TARGETS)

t1_hello:      t1_hello.c      ; $(MPICC) -o $@ $<
t2_malloc:     t2_malloc.c     ; $(MPICC) -o $@ $<
t3_fork:       t3_fork.c       ; $(MPICC) -o $@ $<
t4_collective: t4_collective.c ; $(MPICC) -o $@ $<
t5_manyranks:  t5_manyranks.c  ; $(MPICC) -o $@ $<
t6_thread:     t6_thread.c     ; $(MPICC) -o $@ $< -lpthread

run-t1: t1_hello
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t1_hello

run-t2: t2_malloc
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t2_malloc

run-t3: t3_fork
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t3_fork

run-t4: t4_collective
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t4_collective

run-t5: t5_manyranks
	$(EMP) $(MPIRUN) $(MPIOPT) -np 16 ./t5_manyranks

run-t6: t6_thread
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t6_thread

run-all: all
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t1_hello
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t2_malloc
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t3_fork
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t4_collective
	$(EMP) $(MPIRUN) $(MPIOPT) -np 16 ./t5_manyranks
	$(EMP) $(MPIRUN) $(MPIOPT) -np 4  ./t6_thread

clean:
	rm -f $(TARGETS)

#		     |10.10.0.113:32768:20676|10.10.0.113:32768:20677|10.10.0.113:32768:20678|10.10.0.113:32768:20679|10.10.0.113:32768:20680" \
