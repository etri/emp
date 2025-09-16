#!/bin/bash

# YCSB benchmark path
YCSB_PATH='/mnt/mcanas2/user/benchmark/memcached/YCSB'

# local dram size
LOCAL_SIZE=("$((64<<10))" "$((32<<10))" "$((16<<10))")
LOCAL_SIZE_TXT=("64G" "32G" "16G")
loops_local_size='0 1 2'

# record count and operation count in the workload of YCSB
# recordcount=8,000,000
# operationcount=1000
WORKLOADS=("workloadf.benchmark" "workloadb.benchmark" "workloadc.benchmark" "workloada.benchmark")
loops_workloads='0 1 2 3'

echo "===================================================="
echo "memcached benchmark with emp(dornor = remote memory)"
echo "===================================================="

sudo kill -9 $(pidof memcached)
pushd $YCSB_PATH

for j in $loops_local_size
do
for i in $loops_workloads
do
echo "==========================="
echo ${WORKLOADS[$i]}
echo "==========================="
echo "EMP_MEM_PATH=\"dram:${LOCAL_SIZE[$j]}|10.10.0.121:$((256<<10)):19600\" env LD_PRELOAD=/mnt/mcanas2/user/benchmark/mimalloc-emp/out/release/libmimalloc.so memcached -m $((128<<10)) &"
EMP_MEM_PATH="dram:${LOCAL_SIZE[$j]}|10.10.0.121:$((256<<10)):19600" env LD_PRELOAD=/mnt/mcanas2/user/benchmark/mimalloc-emp/out/release/libmimalloc.so memcached -m $((128<<10)) &
./bin/ycsb load memcached -s -P ./workloads/${WORKLOADS[$i]} -p memcached.hosts=127.0.0.1 -threads 32 > emp_memcached_output_load_${WORKLOADS[$i]}_${LOCAL_SIZE_TXT[$j]}.txt
./bin/ycsb run  memcached -s -P ./workloads/${WORKLOADS[$i]} -p memcached.hosts=127.0.0.1 -threads 32 > emp_memcached_output_run_${WORKLOADS[$i]}_${LOCAL_SIZE_TXT[$j]}.txt
sudo kill -9 $(pidof memcached)
#wait for emp exit
sleep 1m
done
done

popd
